#include "sketch/count_min_sketch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "reference/workloads.h"

namespace lcache {
namespace {

TEST(CountMinSketch, WidthIsAPowerOfTwoWithAMinimum) {
  EXPECT_EQ(CountMinSketch(1).width(), 16U);
  EXPECT_EQ(CountMinSketch(16).width(), 16U);
  EXPECT_EQ(CountMinSketch(100).width(), 128U);
  EXPECT_EQ(CountMinSketch(1024).width(), 1024U);
  EXPECT_EQ(CountMinSketch(1024).sample_size(), 10240U);
}

TEST(CountMinSketch, UnseenKeysEstimateZero) {
  const CountMinSketch s(1024);
  for (Key k = 0; k < 100; ++k) EXPECT_EQ(s.estimate(k), 0U);
}

TEST(CountMinSketch, CountsIncrements) {
  CountMinSketch s(1024);
  for (int i = 0; i < 5; ++i) s.increment(42);
  s.increment(7);
  EXPECT_EQ(s.estimate(42), 5U);
  EXPECT_EQ(s.estimate(7), 1U);
}

TEST(CountMinSketch, SaturatesAtFifteen) {
  CountMinSketch s(1024);
  for (int i = 0; i < 100; ++i) s.increment(1);
  EXPECT_EQ(s.estimate(1), CountMinSketch::kMaxCount);
}

TEST(CountMinSketch, NeverUnderestimatesBeforeAging) {
  // A narrow sketch with many keys forces collisions; estimates may be high, never low.
  CountMinSketch s(64);
  std::unordered_map<Key, int> truth;
  const auto keys = test::zipf_keys(600, 300, 0.8, 3);  // 600 < sample_size() = 640: no aging
  for (const Key k : keys) {
    s.increment(k);
    ++truth[k];
  }
  for (const auto& [k, n] : truth) {
    EXPECT_GE(s.estimate(k), std::min(n, int{CountMinSketch::kMaxCount})) << "key " << k;
  }
}

TEST(CountMinSketch, AgingHalvesCountersAfterSampleSizeIncrements) {
  CountMinSketch s(16);  // sample_size = 160
  for (int i = 0; i < 15; ++i) s.increment(1);
  ASSERT_EQ(s.estimate(1), 15U);
  // 144 more increments of other keys: 159 in total, one short of aging.
  for (Key k = 100; k < 244; ++k) s.increment(k);
  ASSERT_EQ(s.estimate(1), 15U);  // collisions can't push a saturated counter higher
  s.increment(999);               // 160th increment triggers aging
  EXPECT_EQ(s.estimate(1), 7U);   // 15 / 2
}

TEST(CountMinSketch, IsDeterministic) {
  CountMinSketch a(32);
  CountMinSketch b(32);
  const auto keys = test::zipf_keys(5000, 500, 0.9, 4);  // includes several agings
  for (const Key k : keys) {
    a.increment(k);
    b.increment(k);
  }
  for (Key k = 0; k < 500; ++k) ASSERT_EQ(a.estimate(k), b.estimate(k)) << "key " << k;
}

TEST(CountMinSketch, PopularKeysOutrankRareOnesAfterManyAgings) {
  CountMinSketch s(256);
  const auto keys = test::zipf_keys(50'000, 5000, 1.0, 5);
  for (const Key k : keys) s.increment(k);
  // Key 0 is the most popular under Zipf; keys near the tail are rarely seen.
  EXPECT_GT(s.estimate(0), s.estimate(4999));
  EXPECT_GE(s.estimate(0), s.estimate(10));
}

}  // namespace
}  // namespace lcache
