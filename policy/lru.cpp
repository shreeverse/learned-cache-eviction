#include "policy/lru.h"

#include <cassert>

namespace lcache {

void LruPolicy::on_hit(const Request& r) {
  const auto it = index_.find(r.key);
  assert(it != index_.end() && "on_hit: key not tracked");
  // splice relinks the existing node at the front: no allocation, and the iterator in index_
  // stays valid, so the map needs no update.
  order_.splice(order_.begin(), order_, it->second);
}

void LruPolicy::on_insert(const Request& r) {
  assert(!index_.contains(r.key) && "on_insert: key already tracked");
  order_.push_front(r.key);
  index_.emplace(r.key, order_.begin());
}

Key LruPolicy::evict() {
  assert(!order_.empty() && "evict: nothing tracked");
  const Key victim = order_.back();
  order_.pop_back();
  index_.erase(victim);
  return victim;
}

void LruPolicy::on_remove(Key k) {
  const auto it = index_.find(k);
  if (it == index_.end()) return;  // not tracked: no-op by contract
  order_.erase(it->second);
  index_.erase(it);
}

}  // namespace lcache
