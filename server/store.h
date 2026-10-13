#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "policy/eviction_policy.h"
#include "policy/policy_factory.h"
#include "trace/request.h"

namespace lcache::server {

// Maps a key's bytes to the 64-bit Key the policies see (decisions D22). It must be
// deterministic, so a key evicted and SET again gets the same Key and keeps its FeatureTracker
// history (D17, D20). A function pointer so tests can force collisions.
using KeyHasher = Key (*)(std::string_view);

// FNV-1a over the bytes, then the splitmix64 finalizer so every output bit depends on every
// input byte (the shard index uses the top bits).
Key default_key_hash(std::string_view key);

enum class SetResult : std::uint8_t {
  Stored,    // stored (possibly evicted right away by the policy, decisions D2)
  TooLarge,  // key + value exceed the shard's capacity; nothing changed
};

// Counters per shard, updated under the shard lock.
struct StoreStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t sets = 0;
  std::uint64_t evictions = 0;
  std::uint64_t collisions = 0;  // SETs that replaced a different key with the same hash
  std::uint64_t too_large = 0;

  StoreStats& operator+=(const StoreStats& o);
};

// One partition of the store: values, byte accounting and one eviction policy behind a mutex.
// The cache rules are sim::Cache's (D1-D3), with cache-aside semantics: a GET miss doesn't
// insert, a SET does (docs/m5-design.md section 5).
//
// Entries are keyed by the 64-bit id alone and hold the full key: the policy speaks ids, so a
// victim id finds its entry directly, and GET compares the full key, so a colliding key reads
// as a miss. Two keys with one id can't both be stored; the newer SET replaces the older entry.
//
// The charge for an entry is key.size() + value.size(). Thread-safe; every method locks.
// alignas(64): neighbouring shards' mutexes don't share a cache line.
class alignas(64) Shard {
 public:
  // Throws std::invalid_argument if policy is null.
  Shard(std::unique_ptr<EvictionPolicy> policy, std::uint64_t capacity_bytes);

  // On a hit, calls on_value(value) under the lock (so the caller can copy the value straight
  // into its reply buffer) and returns true. `on_value` must not call back into the store.
  bool get(std::string_view key, Key id, const std::function<void(std::string_view)>& on_value);
  SetResult set(std::string_view key, Key id, std::string_view value);
  // Returns true if `key` was stored.
  bool del(std::string_view key, Key id);

  std::uint64_t used_bytes() const;
  std::uint64_t capacity_bytes() const { return capacity_bytes_; }  // immutable
  std::size_t size() const;
  StoreStats stats() const;

 private:
  struct Entry {
    std::string key;
    std::string value;
    std::uint64_t charge() const { return key.size() + value.size(); }
  };

  Request request(Key id, std::uint64_t charge, Op op);
  void evict_until_fits();  // needs mu_

  const std::uint64_t capacity_bytes_;
  mutable std::mutex mu_;
  std::unique_ptr<EvictionPolicy> policy_;  // guarded by mu_
  std::unordered_map<Key, Entry> entries_;  // guarded by mu_
  std::uint64_t used_bytes_ = 0;            // guarded by mu_
  std::uint64_t clock_ = 0;                 // guarded by mu_; Request::time for the policy
  StoreStats stats_;                        // guarded by mu_
};

// Creates the policy for one shard (index `shard` of the store) with the given capacity.
using PolicyMaker =
    std::function<std::unique_ptr<EvictionPolicy>(std::uint64_t capacity_bytes, std::size_t shard)>;

// A PolicyMaker for make_policy(name, config) with config.capacity_bytes set per shard and
// config.seed + shard as the seed. Checks the config once, eagerly: throws
// std::invalid_argument for an unknown policy or one that needs future knowledge (Belady,
// oracle-sampled), which a server can't provide.
PolicyMaker make_policy_maker(std::string name, PolicyConfig config);

// The server's key-value store: S shards, each with capacity total / S, chosen by the top bits
// of the key's hash. Thread-safe.
class ShardedStore {
 public:
  // Throws std::invalid_argument unless shards is a power of two and capacity_bytes >= shards.
  ShardedStore(std::uint64_t capacity_bytes, std::size_t shards, const PolicyMaker& make_policy,
               KeyHasher hasher = default_key_hash);

  bool get(std::string_view key, const std::function<void(std::string_view)>& on_value);
  std::optional<std::string> get(std::string_view key);  // copies; for tests and tools
  SetResult set(std::string_view key, std::string_view value);
  bool del(std::string_view key);

  std::size_t shard_count() const { return shards_.size(); }
  std::size_t shard_of(std::string_view key) const { return shard_index(hasher_(key)); }
  const Shard& shard(std::size_t i) const { return *shards_.at(i); }
  // Sums over shards; each shard is read under its own lock, so not an atomic snapshot.
  StoreStats stats() const;

 private:
  std::size_t shard_index(Key id) const;

  KeyHasher hasher_;
  unsigned shard_shift_ = 0;  // 64 - log2(shards); unused when there is one shard
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace lcache::server
