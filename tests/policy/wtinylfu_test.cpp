#include "policy/wtinylfu.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "policy/lru.h"
#include "reference/workloads.h"
#include "sim/cache.h"

namespace lcache {
namespace {

using test::req;

// A sketch wide enough that it never resizes (resizing forgets counts) and collisions are
// negligible in these small tests, so frequencies equal true access counts.
WTinyLfuPolicy::Options exact_sketch() { return {.initial_sketch_width = 4096}; }

TEST(WTinyLfuPolicy, Name) {
  WTinyLfuPolicy p(100);
  EXPECT_EQ(p.name(), "wtinylfu");
}

TEST(WTinyLfuPolicy, SegmentBudgets) {
  WTinyLfuPolicy p(1000);
  EXPECT_EQ(p.window_budget(), 10U);      // 1% of 1000
  EXPECT_EQ(p.protected_budget(), 792U);  // 80% of the 990-byte main area
}

TEST(WTinyLfuPolicy, NewKeysEnterTheWindowAndOverflowMovesToProbation) {
  WTinyLfuPolicy p(100, exact_sketch());  // window budget 1
  Cache cache(p, 100);
  cache.access(req(1));
  EXPECT_EQ(p.state().window, 1U);
  cache.access(req(2));  // window overflows: 1 moves to probation (main has room, no eviction)
  EXPECT_EQ(p.state().window, 1U);
  EXPECT_EQ(p.state().probation, 1U);
  EXPECT_TRUE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
}

TEST(WTinyLfuPolicy, EveryAccessIsCountedInTheSketch) {
  WTinyLfuPolicy p(100, exact_sketch());
  Cache cache(p, 100);
  cache.access(req(7));
  cache.access(req(7));
  cache.access(req(7));
  EXPECT_EQ(p.frequency(7), 3U);
  EXPECT_EQ(p.frequency(8), 0U);
}

TEST(WTinyLfuPolicy, ProbationHitPromotesToProtected) {
  WTinyLfuPolicy p(100, exact_sketch());
  Cache cache(p, 100);
  cache.access(req(1));
  cache.access(req(2));  // 1 -> probation
  ASSERT_EQ(p.state().probation, 1U);
  EXPECT_TRUE(cache.access(req(1)));
  EXPECT_EQ(p.state().probation, 0U);
  EXPECT_EQ(p.state().protected_bytes, 1U);
}

TEST(WTinyLfuPolicy, ProtectedOverflowIsDemotedNotEvicted) {
  WTinyLfuPolicy p(100, exact_sketch());  // window 1, protected budget 79
  Cache cache(p, 100);
  for (Key k = 0; k < 100; ++k) cache.access(req(k));  // exactly full: window [99], probation 0..98
  for (Key k = 0; k < 80; ++k) EXPECT_TRUE(cache.access(req(k)));  // 80 promotions
  const auto s = p.state();
  EXPECT_EQ(s.protected_bytes, 79U);  // key 0 (protected LRU) was demoted
  EXPECT_EQ(s.probation, 20U);
  EXPECT_EQ(s.window, 1U);
  EXPECT_EQ(cache.object_count(), 100U);  // nothing evicted
}

TEST(WTinyLfuPolicy, CandidateWithEqualFrequencyIsRejected) {
  WTinyLfuPolicy p(100, exact_sketch());
  Cache cache(p, 100);
  for (Key k = 0; k < 100; ++k) cache.access(req(k));  // all frequency 1; window [99]
  cache.access(req(1000));  // 99 becomes the candidate vs victim 0 (probation LRU): tie
  EXPECT_FALSE(cache.contains(99));
  EXPECT_TRUE(cache.contains(0));
  EXPECT_TRUE(cache.contains(1000));  // the new key sits in the window
}

TEST(WTinyLfuPolicy, MoreFrequentCandidateIsAdmittedAndVictimEvicted) {
  WTinyLfuPolicy p(100, exact_sketch());
  Cache cache(p, 100);
  for (Key k = 0; k < 99; ++k) cache.access(req(k));  // window [98], probation 0..97
  cache.access(req(500));  // 98 -> probation (no eviction yet: 100 objects)
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(cache.access(req(500)));  // window hits: frequency 4
  cache.access(req(1000));  // candidate 500 (freq 4) vs victim 0 (freq 1)
  EXPECT_TRUE(cache.contains(500));
  EXPECT_FALSE(cache.contains(0));
}

TEST(WTinyLfuPolicy, NewcomerLargerThanTheWindowCanBeRejected) {
  // Capacity 1000 bytes: window budget 10. A 20-byte newcomer overflows the window by itself, so
  // it goes straight to an admission duel, which it loses on a tie.
  WTinyLfuPolicy p(1000, exact_sketch());
  Cache cache(p, 1000);
  for (Key k = 0; k < 100; ++k) cache.access(req(k, 10));  // full: window [99], probation 0..98
  EXPECT_FALSE(cache.access(req(500, 20)));
  EXPECT_FALSE(cache.contains(500));
  EXPECT_FALSE(cache.contains(99));  // the older window key also lost its duel
  EXPECT_TRUE(cache.contains(0));
  EXPECT_EQ(cache.used_bytes(), 990U);
}

TEST(WTinyLfuPolicy, RemoveFromEachSegment) {
  WTinyLfuPolicy p(100, exact_sketch());
  Cache cache(p, 100);
  cache.access(req(1));
  cache.access(req(2));
  cache.access(req(3));
  cache.access(req(1));  // window [3], probation [2], protected [1]
  p.on_remove(3);
  p.on_remove(2);
  p.on_remove(1);
  p.on_remove(99);  // unknown: no-op
  const auto s = p.state();
  EXPECT_EQ(s.window + s.probation + s.protected_bytes, 0U);
}

TEST(WTinyLfuPolicy, SketchGrowsWithTheNumberOfResidentKeys) {
  WTinyLfuPolicy p(1000);
  Cache cache(p, 1000);
  EXPECT_EQ(p.state().sketch_width, 16U);
  for (Key k = 0; k < 100; ++k) cache.access(req(k));
  EXPECT_GE(p.state().sketch_width, 128U);
}

TEST(WTinyLfuPolicy, ByteInvariantsHoldWithVariableSizes) {
  std::mt19937_64 rng(21);
  std::uniform_int_distribution<std::uint32_t> pick_size(1, 200);
  const auto keys = test::zipf_keys(30'000, 2000, 0.9, 22);
  constexpr std::uint64_t kCap = 20'000;
  WTinyLfuPolicy p(kCap);
  Cache cache(p, kCap);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    cache.access(req(keys[i], pick_size(rng)));
    const auto s = p.state();
    ASSERT_EQ(s.window + s.probation + s.protected_bytes, cache.used_bytes()) << "step " << i;
    ASSERT_LE(s.window, p.window_budget()) << "step " << i;
    ASSERT_LE(s.protected_bytes, p.protected_budget()) << "step " << i;
  }
}

// ---- Workload properties (fixed seeds, so deterministic) -----------------------------------

double hit_rate(EvictionPolicy& policy, std::uint64_t capacity, const std::vector<Key>& keys) {
  Cache cache(policy, capacity);
  std::size_t hits = 0;
  for (const Key k : keys) hits += cache.access(req(k)) ? 1U : 0U;
  return static_cast<double>(hits) / static_cast<double>(keys.size());
}

TEST(WTinyLfuPolicy, ProtectedHotKeysSurviveAScanThatFlushesLru) {
  // Ten hot keys are accessed 5 times each, then a 10,000-key one-time scan. Keys 0..8 end up in
  // the protected segment; scan keys only ever duel against probation, so they survive. Key 9 is
  // the window's MRU when the scan starts; the window is plain LRU by design, so it isn't
  // guaranteed to survive and isn't counted.
  const auto run = [](EvictionPolicy& policy) {
    Cache cache(policy, 100);
    for (int round = 0; round < 5; ++round) {
      for (Key k = 0; k < 10; ++k) cache.access(req(k));
    }
    for (Key k = 1000; k < 11'000; ++k) cache.access(req(k));
    int hits = 0;
    for (Key k = 0; k < 9; ++k) hits += cache.access(req(k)) ? 1 : 0;
    return hits;
  };
  WTinyLfuPolicy wtinylfu(100);
  LruPolicy lru;
  EXPECT_EQ(run(wtinylfu), 9);
  EXPECT_EQ(run(lru), 0);
}

TEST(WTinyLfuPolicy, BeatsLruOnZipf) {
  const auto keys = test::zipf_keys(200'000, 10'000, 0.9, 23);
  WTinyLfuPolicy wtinylfu(500);
  LruPolicy lru;
  EXPECT_GT(hit_rate(wtinylfu, 500, keys), hit_rate(lru, 500, keys));
}

}  // namespace
}  // namespace lcache
