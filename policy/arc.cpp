#include "policy/arc.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace lcache {

void ArcPolicy::on_hit(const Request& r) {
  Entry& e = index_.at(r.key);
  assert((e.list == kT1 || e.list == kT2) && "on_hit: key not resident");
  newcomer_.reset();
  move_to(e, kT2);  // Case I: any hit makes the key "frequent"
}

void ArcPolicy::on_insert(const Request& r) {
  auto it = index_.find(r.key);
  newcomer_ = r.key;
  newcomer_from_b2_ = false;
  const auto size = static_cast<double>(r.size);

  if (it == index_.end()) {
    // Case IV: never seen (or ghost already trimmed) -> T1.
    lists_[kT1].push_back(r.key);
    bytes_[kT1] += r.size;
    index_.emplace(r.key, Entry{kT1, std::prev(lists_[kT1].end()), r.size});
  } else {
    assert((it->second.list == kB1 || it->second.list == kB2) && "on_insert: key already resident");
    const auto b1 = static_cast<double>(bytes_[kB1]);
    const auto b2 = static_cast<double>(bytes_[kB2]);
    if (it->second.list == kB1) {
      // Case II: we evicted it from T1 too early -> grow T1's target. For unit sizes
      // size * max(1, |B2|/|B1|) is the paper's delta1.
      p_ = std::min(p_ + size * std::max(1.0, b2 / b1), static_cast<double>(c_));
    } else {
      // Case III: we evicted it from T2 too early -> shrink T1's target.
      p_ = std::max(p_ - size * std::max(1.0, b1 / b2), 0.0);
      newcomer_from_b2_ = true;
    }
    // The ghost comes back with the size of this request.
    bytes_[it->second.list] -= it->second.size;
    it->second.size = r.size;
    bytes_[it->second.list] += r.size;
    move_to(it->second, kT2);
  }
  trim_ghosts();
}

Key ArcPolicy::evict() {
  assert(bytes_[kT1] + bytes_[kT2] > 0 && "evict: nothing resident");
  // REPLACE(x, p), computed as if the newcomer x had not been added yet (m2-design 0.2).
  const auto newcomer_bytes = [&](ListId l) -> std::uint64_t {
    if (!newcomer_) return 0;
    const Entry& e = index_.at(*newcomer_);
    return e.list == l ? e.size : 0;
  };
  const std::uint64_t t1 = bytes_[kT1] - newcomer_bytes(kT1);
  const std::uint64_t t2 = bytes_[kT2] - newcomer_bytes(kT2);
  const auto t1d = static_cast<double>(t1);

  ListId from = kT1;
  if (t1 > 0 && (t1d > p_ || (newcomer_from_b2_ && t1d == p_))) {
    from = kT1;
  } else if (t2 > 0) {
    from = kT2;
  }  // else: only T1 has anything besides the newcomer (fallback, as in the reference)

  // The LRU end is never the newcomer while its list holds anything else, since the newcomer is
  // at the MRU end.
  const Key victim = lists_[from].front();
  move_to(index_.at(victim), from == kT1 ? kB1 : kB2);
  if (newcomer_ == victim) newcomer_.reset();
  trim_ghosts();
  return victim;
}

void ArcPolicy::on_remove(Key k) {
  const auto it = index_.find(k);
  if (it == index_.end() || it->second.list == kB1 || it->second.list == kB2) return;
  if (newcomer_ == k) newcomer_.reset();
  drop(k);  // explicit delete: no ghost, the key wasn't evicted for lack of space
}

ArcPolicy::State ArcPolicy::state() const {
  return State{p_, bytes_[kT1], bytes_[kT2], bytes_[kB1], bytes_[kB2]};
}

void ArcPolicy::move_to(Entry& e, ListId to) {
  bytes_[e.list] -= e.size;
  lists_[to].splice(lists_[to].end(), lists_[e.list], e.pos);  // node moves; e.pos stays valid
  e.list = to;
  bytes_[to] += e.size;
}

void ArcPolicy::drop(Key k) {
  const Entry& e = index_.at(k);
  bytes_[e.list] -= e.size;
  lists_[e.list].erase(e.pos);
  index_.erase(k);
}

void ArcPolicy::trim_ghosts() {
  // Only trim once the resident lists fit. While the newcomer still has to be paid for by an
  // eviction, trimming now would drop ghosts the paper keeps (e.g. Case IV-A with |T1| = c, where
  // the paper deletes T1's LRU without touching B2).
  if (bytes_[kT1] + bytes_[kT2] > c_) return;
  while (bytes_[kT1] + bytes_[kB1] > c_ && !lists_[kB1].empty()) {
    drop(lists_[kB1].front());
  }
  const auto total = [&] { return bytes_[kT1] + bytes_[kT2] + bytes_[kB1] + bytes_[kB2]; };
  while (total() > 2 * c_ && !(lists_[kB1].empty() && lists_[kB2].empty())) {
    const ListId from = lists_[kB2].empty() ? kB1 : kB2;
    drop(lists_[from].front());
  }
}

}  // namespace lcache
