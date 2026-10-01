#include "policy/wtinylfu.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace lcache {

namespace {

std::uint64_t fraction_of(std::uint64_t bytes, double fraction) {
  return static_cast<std::uint64_t>(static_cast<double>(bytes) * fraction);
}

}  // namespace

WTinyLfuPolicy::WTinyLfuPolicy(std::uint64_t capacity_bytes, const Options& options)
    : window_budget_(fraction_of(capacity_bytes, options.window_fraction)),
      protected_budget_(fraction_of(capacity_bytes - window_budget_, options.protected_fraction)),
      sketch_(options.initial_sketch_width) {}

void WTinyLfuPolicy::on_hit(const Request& r) {
  pending_.clear();  // a new access: last access's candidates are now ordinary probation keys
  record_access(r.key);
  Entry& e = index_.at(r.key);
  switch (e.segment) {
    case kWindow:
    case kProtected:
      move_to(e, e.segment);  // refresh recency within the segment
      break;
    case kProbation:
      move_to(e, kProtected);  // second chance earned: promote
      demote_protected_overflow();
      break;
  }
}

void WTinyLfuPolicy::on_insert(const Request& r) {
  pending_.clear();
  record_access(r.key);
  assert(!index_.contains(r.key) && "on_insert: key already tracked");
  lists_[kWindow].push_back(r.key);
  bytes_[kWindow] += r.size;
  index_.emplace(r.key, Entry{kWindow, std::prev(lists_[kWindow].end()), r.size});

  if (index_.size() > sketch_.width()) {
    // Rebuild wider; old counts are lost (only O(log n) times, during warm-up).
    sketch_ = CountMinSketch(sketch_.width() * 2);
  }
  drain_window();
}

Key WTinyLfuPolicy::evict() {
  assert(!index_.empty() && "evict: nothing tracked");
  drain_window();

  Key evicted = 0;
  if (!pending_.empty()) {
    const Key candidate = pending_.front();
    // TinyLFU admission: the candidate must be strictly more popular than main's victim.
    if (has_main_victim() && sketch_.estimate(candidate) > sketch_.estimate(main_victim())) {
      evicted = main_victim();  // the candidate stays pending and duels again if still needed
    } else {
      evicted = candidate;
      pending_.pop_front();
    }
  } else if (has_main_victim()) {
    evicted = main_victim();
  } else {
    evicted = lists_[kWindow].front();  // main is empty: fall back to the window's LRU
  }
  drop(evicted);
  return evicted;
}

void WTinyLfuPolicy::on_remove(Key k) {
  if (!index_.contains(k)) return;
  std::erase(pending_, k);
  drop(k);
}

WTinyLfuPolicy::State WTinyLfuPolicy::state() const {
  return State{bytes_[kWindow], bytes_[kProbation], bytes_[kProtected], sketch_.width()};
}

void WTinyLfuPolicy::record_access(Key k) { sketch_.increment(k); }

void WTinyLfuPolicy::move_to(Entry& e, Segment to) {
  bytes_[e.segment] -= e.size;
  lists_[to].splice(lists_[to].end(), lists_[e.segment], e.pos);  // e.pos stays valid
  e.segment = to;
  bytes_[to] += e.size;
}

void WTinyLfuPolicy::drop(Key k) {
  const Entry& e = index_.at(k);
  bytes_[e.segment] -= e.size;
  lists_[e.segment].erase(e.pos);
  index_.erase(k);
}

void WTinyLfuPolicy::drain_window() {
  while (bytes_[kWindow] > window_budget_ && !lists_[kWindow].empty()) {
    const Key k = lists_[kWindow].front();
    move_to(index_.at(k), kProbation);
    pending_.push_back(k);
  }
}

void WTinyLfuPolicy::demote_protected_overflow() {
  while (bytes_[kProtected] > protected_budget_ && !lists_[kProtected].empty()) {
    move_to(index_.at(lists_[kProtected].front()), kProbation);
  }
}

// Pending candidates were appended to probation's MRU end during this access, so probation holds
// a non-candidate key exactly when it has more keys than there are candidates, and that key is
// at the LRU end.
bool WTinyLfuPolicy::has_main_victim() const {
  return lists_[kProbation].size() > pending_.size() || !lists_[kProtected].empty();
}

Key WTinyLfuPolicy::main_victim() const {
  return lists_[kProbation].size() > pending_.size() ? lists_[kProbation].front()
                                                     : lists_[kProtected].front();
}

}  // namespace lcache
