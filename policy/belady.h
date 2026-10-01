#pragma once

#include <cstdint>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "policy/eviction_policy.h"

namespace lcache {

// Belady's MIN: evict the object whose next access (Request::next_vtime) is furthest in the
// future; objects never accessed again go first. Because the Cache inserts before evicting, the
// newcomer competes too, so Belady can decline to keep an object it won't need ("bypass").
//
// For unit-size objects this is the offline optimum (checked against exhaustive search in the
// tests). For variable sizes, furthest-next-access is a heuristic, not the byte-optimal policy
// (which is NP-hard); results call it "Belady (furthest next access)".
//
// Needs future knowledge: simulate() annotates the trace (needs_next_access() is true).
// O(log n) per hook (ordered set). Ties are broken by key, so results are deterministic.
class BeladyPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "belady"; }
  bool needs_next_access() const override { return true; }

 private:
  std::set<std::pair<std::uint64_t, Key>> by_next_;  // (next_vtime, key); last = victim
  std::unordered_map<Key, std::uint64_t> next_;      // key -> its next_vtime in by_next_
};

}  // namespace lcache
