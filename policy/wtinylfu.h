#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <string_view>
#include <unordered_map>

#include "policy/eviction_policy.h"
#include "sketch/count_min_sketch.h"

namespace lcache {

// W-TinyLFU (Einziger, Friedman & Manes 2017), following Caffeine's structure, in bytes:
//   window LRU (1%)  ->  main SLRU = probation (20% of main) + protected (80% of main)
// with a count-min sketch (TinyLFU) deciding admission into main.
//
// - Every access increments the key in the sketch.
// - New keys enter the window. When the window is over budget its LRU keys move to probation as
//   pending *candidates*. If the cache is then over capacity, evict() duels the oldest candidate
//   against main's victim (probation LRU, else protected LRU): the candidate is admitted only if
//   its estimated frequency is strictly higher; otherwise the candidate itself is evicted. A
//   newcomer larger than the window becomes a candidate at once, so it can be rejected.
// - Probation hit -> protected; protected overflow demotes its LRU to probation's MRU end.
//
// The sketch starts at Options::initial_sketch_width and doubles (losing its counts) whenever
// the number of resident keys exceeds its width, since a byte-sized cache doesn't know its object
// count up front (docs/m2-design.md section 3).
//
// O(1) average per hook. Not implemented (see decisions log): Caffeine's adaptive window
// (hill climbing), the doorkeeper filter, and random admission against hash flooding.
class WTinyLfuPolicy final : public EvictionPolicy {
 public:
  struct Options {
    double window_fraction = 0.01;     // of capacity
    double protected_fraction = 0.80;  // of the main area (capacity - window)
    std::size_t initial_sketch_width = CountMinSketch::kMinWidth;
  };

  explicit WTinyLfuPolicy(std::uint64_t capacity_bytes)
      : WTinyLfuPolicy(capacity_bytes, Options{}) {}
  WTinyLfuPolicy(std::uint64_t capacity_bytes, const Options& options);

  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "wtinylfu"; }

  // For tests and debugging.
  struct State {
    std::uint64_t window, probation, protected_bytes;
    std::size_t sketch_width;
  };
  State state() const;
  std::uint8_t frequency(Key k) const { return sketch_.estimate(k); }
  std::uint64_t window_budget() const { return window_budget_; }
  std::uint64_t protected_budget() const { return protected_budget_; }

 private:
  enum Segment : std::uint8_t { kWindow = 0, kProbation = 1, kProtected = 2 };
  struct Entry {
    Segment segment;
    std::list<Key>::iterator pos;
    std::uint32_t size;
  };

  void record_access(Key k);
  void move_to(Entry& e, Segment to);  // to the MRU end of `to`
  void drop(Key k);                    // remove from its segment and the index
  void drain_window();                 // window overflow -> probation, as pending candidates
  void demote_protected_overflow();    // protected overflow -> probation MRU
  Key main_victim() const;             // oldest non-pending main key; requires one to exist
  bool has_main_victim() const;

  std::uint64_t window_budget_;
  std::uint64_t protected_budget_;
  CountMinSketch sketch_;
  std::array<std::list<Key>, 3> lists_;  // front = LRU, back = MRU
  std::array<std::uint64_t, 3> bytes_{};
  std::unordered_map<Key, Entry> index_;
  std::deque<Key> pending_;  // candidates moved from the window during the current access
};

}  // namespace lcache
