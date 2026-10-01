#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "policy/eviction_policy.h"
#include "trace/request.h"

namespace lcache {

// A byte-capacity cache that stores only keys and sizes (no values). It owns all capacity
// accounting and delegates the choice of victim to an EvictionPolicy (docs/decisions.md D1-D3),
// so every policy is evaluated under exactly the same rules.
//
// The policy is not owned; it must outlive the Cache, start out empty, and not be shared.
class Cache {
 public:
  Cache(EvictionPolicy& policy, std::uint64_t capacity_bytes);

  // Processes one request (treated as a GET) and returns true on a hit.
  //   - Hit:  calls policy.on_hit(r). The stored size is kept even if r.size differs.
  //   - Miss, r.size > capacity: nothing is inserted and the policy is not called.
  //   - Miss otherwise: insert r, call policy.on_insert(r), then call policy.evict() and drop
  //     the victim until used_bytes() <= capacity_bytes(). The victim may be r itself.
  // Throws std::logic_error if the policy evicts a key the cache does not hold (a policy bug).
  bool access(const Request& r);

  bool contains(Key k) const { return sizes_.contains(k); }
  std::uint64_t used_bytes() const { return used_bytes_; }
  std::uint64_t capacity_bytes() const { return capacity_bytes_; }
  std::size_t object_count() const { return sizes_.size(); }

 private:
  EvictionPolicy& policy_;
  std::uint64_t capacity_bytes_;
  std::uint64_t used_bytes_ = 0;
  std::unordered_map<Key, std::uint32_t> sizes_;  // cached key -> size charged for it
};

}  // namespace lcache
