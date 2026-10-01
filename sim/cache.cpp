#include "sim/cache.h"

#include <stdexcept>
#include <string>

namespace lcache {

Cache::Cache(EvictionPolicy& policy, std::uint64_t capacity_bytes)
    : policy_(policy), capacity_bytes_(capacity_bytes) {}

bool Cache::access(const Request& r) {
  if (sizes_.contains(r.key)) {
    policy_.on_hit(r);  // keep the stored size even if r.size differs (decisions.md D3)
    return true;
  }
  if (r.size > capacity_bytes_) {
    return false;  // can never fit; don't disturb the current contents
  }

  // Insert first, then evict until we fit, so the policy may pick the newcomer (D2).
  sizes_.emplace(r.key, r.size);
  used_bytes_ += r.size;
  policy_.on_insert(r);

  while (used_bytes_ > capacity_bytes_) {
    const Key victim = policy_.evict();
    const auto it = sizes_.find(victim);
    if (it == sizes_.end()) {
      throw std::logic_error("policy '" + std::string(policy_.name()) +
                             "' evicted a key that is not cached: " + std::to_string(victim));
    }
    used_bytes_ -= it->second;
    sizes_.erase(it);
  }
  return false;
}

}  // namespace lcache
