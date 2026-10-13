#include "server/store.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace lcache::server {

Key default_key_hash(std::string_view key) {
  std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64-bit offset basis
  for (const char c : key) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;  // FNV prime
  }
  // splitmix64 finalizer: FNV-1a's high bits mix poorly for short keys.
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

StoreStats& StoreStats::operator+=(const StoreStats& o) {
  hits += o.hits;
  misses += o.misses;
  sets += o.sets;
  evictions += o.evictions;
  collisions += o.collisions;
  too_large += o.too_large;
  return *this;
}

// ---- Shard -----------------------------------------------------------------------------------

Shard::Shard(std::unique_ptr<EvictionPolicy> policy, std::uint64_t capacity_bytes)
    : capacity_bytes_(capacity_bytes), policy_(std::move(policy)) {
  if (!policy_) throw std::invalid_argument("Shard: policy is null");
}

Request Shard::request(Key id, std::uint64_t charge, Op op) {
  // charge <= capacity, and set() rejects charges that don't fit Request::size.
  return Request{.time = clock_++,
                 .key = id,
                 .size = static_cast<std::uint32_t>(charge),
                 .op = op,
                 .next_vtime = kNever};
}

bool Shard::get(std::string_view key, Key id,
                const std::function<void(std::string_view)>& on_value) {
  const std::lock_guard lock(mu_);
  const auto it = entries_.find(id);
  if (it == entries_.end() || it->second.key != key) {  // absent, or a colliding key
    ++stats_.misses;
    return false;
  }
  ++stats_.hits;
  policy_->on_hit(request(id, it->second.charge(), Op::Get));
  on_value(it->second.value);
  return true;
}

SetResult Shard::set(std::string_view key, Key id, std::string_view value) {
  const std::uint64_t charge = key.size() + value.size();
  const std::lock_guard lock(mu_);
  if (charge > capacity_bytes_ || charge > std::numeric_limits<std::uint32_t>::max()) {
    ++stats_.too_large;  // like sim::Cache: never stored, current contents left alone (D2)
    return SetResult::TooLarge;
  }
  ++stats_.sets;

  const auto it = entries_.find(id);
  if (it != entries_.end() && it->second.key == key) {
    // Overwrite. The policy keeps the first size, as in the simulator (D3); our accounting
    // uses the new one, so growing a value can trigger evictions.
    used_bytes_ = used_bytes_ - it->second.charge() + charge;
    it->second.value.assign(value);
    policy_->on_hit(request(id, charge, Op::Set));
  } else {
    if (it != entries_.end()) {
      // Hash collision: a different key holds this id. Drop it (legal for a cache), so the
      // policy sees a remove and then a fresh insert.
      ++stats_.collisions;
      used_bytes_ -= it->second.charge();
      policy_->on_remove(id);
      entries_.erase(it);
    }
    entries_.emplace(id, Entry{.key = std::string(key), .value = std::string(value)});
    used_bytes_ += charge;
    policy_->on_insert(request(id, charge, Op::Set));
  }
  evict_until_fits();
  return SetResult::Stored;
}

void Shard::evict_until_fits() {
  while (used_bytes_ > capacity_bytes_) {
    const Key victim = policy_->evict();
    const auto it = entries_.find(victim);
    if (it == entries_.end()) {
      throw std::logic_error("policy '" + std::string(policy_->name()) +
                             "' evicted a key that is not stored: " + std::to_string(victim));
    }
    used_bytes_ -= it->second.charge();
    entries_.erase(it);
    ++stats_.evictions;
  }
}

bool Shard::del(std::string_view key, Key id) {
  const std::lock_guard lock(mu_);
  const auto it = entries_.find(id);
  if (it == entries_.end() || it->second.key != key) return false;
  used_bytes_ -= it->second.charge();
  entries_.erase(it);
  policy_->on_remove(id);
  return true;
}

std::uint64_t Shard::used_bytes() const {
  const std::lock_guard lock(mu_);
  return used_bytes_;
}

std::size_t Shard::size() const {
  const std::lock_guard lock(mu_);
  return entries_.size();
}

StoreStats Shard::stats() const {
  const std::lock_guard lock(mu_);
  return stats_;
}

void Shard::visit_policy(const std::function<void(const EvictionPolicy&)>& f) const {
  const std::lock_guard lock(mu_);
  f(*policy_);
}

// ---- ShardedStore ----------------------------------------------------------------------------

PolicyMaker make_policy_maker(std::string name, PolicyConfig config) {
  // Build one now, so a bad name or config fails at startup rather than per shard.
  const auto probe = make_policy(name, config);
  if (probe->needs_next_access()) {
    throw std::invalid_argument("policy '" + name +
                                "' needs future knowledge and can't run in a server");
  }
  return [name = std::move(name), config = std::move(config)](std::uint64_t capacity_bytes,
                                                              std::size_t shard) {
    PolicyConfig c = config;
    c.capacity_bytes = capacity_bytes;
    c.seed = config.seed + shard;
    return make_policy(name, c);
  };
}

ShardedStore::ShardedStore(std::uint64_t capacity_bytes, std::size_t shards,
                           const PolicyMaker& make_policy, KeyHasher hasher)
    : hasher_(hasher) {
  if (shards == 0 || !std::has_single_bit(shards)) {
    throw std::invalid_argument("ShardedStore: shard count must be a power of two");
  }
  if (capacity_bytes < shards) {
    throw std::invalid_argument("ShardedStore: capacity must be at least one byte per shard");
  }
  if (hasher_ == nullptr) throw std::invalid_argument("ShardedStore: hasher is null");
  shard_shift_ = 64U - static_cast<unsigned>(std::countr_zero(shards));
  const std::uint64_t per_shard = capacity_bytes / shards;  // the remainder is unused
  shards_.reserve(shards);
  for (std::size_t i = 0; i < shards; ++i) {
    shards_.push_back(std::make_unique<Shard>(make_policy(per_shard, i), per_shard));
  }
}

std::size_t ShardedStore::shard_index(Key id) const {
  // Top bits; the shift would be 64 (undefined) for a single shard.
  return shards_.size() == 1 ? 0 : static_cast<std::size_t>(id >> shard_shift_);
}

bool ShardedStore::get(std::string_view key,
                       const std::function<void(std::string_view)>& on_value) {
  const Key id = hasher_(key);
  return shards_[shard_index(id)]->get(key, id, on_value);
}

std::optional<std::string> ShardedStore::get(std::string_view key) {
  std::optional<std::string> out;
  get(key, [&out](std::string_view v) { out.emplace(v); });
  return out;
}

SetResult ShardedStore::set(std::string_view key, std::string_view value) {
  const Key id = hasher_(key);
  return shards_[shard_index(id)]->set(key, id, value);
}

bool ShardedStore::del(std::string_view key) {
  const Key id = hasher_(key);
  return shards_[shard_index(id)]->del(key, id);
}

StoreStats ShardedStore::stats() const {
  StoreStats total;
  for (const auto& s : shards_) total += s->stats();
  return total;
}

void ShardedStore::visit_policies(const std::function<void(const EvictionPolicy&)>& f) const {
  for (const auto& s : shards_) s->visit_policy(f);
}

}  // namespace lcache::server
