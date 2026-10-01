#include "policy/belady.h"

#include <cassert>
#include <iterator>

namespace lcache {

void BeladyPolicy::on_hit(const Request& r) {
  std::uint64_t& next = next_.at(r.key);
  by_next_.erase({next, r.key});
  next = r.next_vtime;  // the request's own annotation: when this key is needed after now
  by_next_.emplace(next, r.key);
}

void BeladyPolicy::on_insert(const Request& r) {
  assert(!next_.contains(r.key) && "on_insert: key already tracked");
  next_.emplace(r.key, r.next_vtime);
  by_next_.emplace(r.next_vtime, r.key);
}

Key BeladyPolicy::evict() {
  assert(!by_next_.empty() && "evict: nothing tracked");
  const auto last = std::prev(by_next_.end());  // furthest next access (kNever sorts last)
  const Key victim = last->second;
  by_next_.erase(last);
  next_.erase(victim);
  return victim;
}

void BeladyPolicy::on_remove(Key k) {
  const auto it = next_.find(k);
  if (it == next_.end()) return;
  by_next_.erase({it->second, k});
  next_.erase(it);
}

}  // namespace lcache
