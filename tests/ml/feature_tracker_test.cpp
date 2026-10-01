#include "ml/feature_tracker.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace lcache {
namespace {

using FT = FeatureTracker;
using Row = std::array<float, FT::kNumFeatures>;

Row features(const FT& ft, Key k) {
  Row row{};
  ft.write_features(k, row);
  return row;
}

// Advances the clock by n accesses to a filler key that the tests never inspect.
void tick(FT& ft, int n) {
  for (int i = 0; i < n; ++i) ft.on_access(999'999, 1);
}

TEST(FeatureTracker, NamesMatchTheColumnLayout) {
  const auto names = FT::feature_names();
  ASSERT_EQ(names.size(), 15U);
  EXPECT_EQ(names[FT::kLogSize], "log_size");
  EXPECT_EQ(names[FT::kAge], "age");
  EXPECT_EQ(names[FT::kDelta0], "delta_1");
  EXPECT_EQ(names[FT::kDelta0 + 7], "delta_8");
  EXPECT_EQ(names[FT::kCount], "count");
  EXPECT_EQ(names[FT::kEdc0], "edc_1");
  EXPECT_EQ(names[FT::kEdc0 + 3], "edc_4");
  EXPECT_EQ(std::set<std::string_view>(names.begin(), names.end()).size(), names.size());
}

TEST(FeatureTracker, FirstAccess) {
  FT ft;
  ft.on_access(7, 1024);
  const Row f = features(ft, 7);
  EXPECT_FLOAT_EQ(f[FT::kLogSize], 10.0F);
  EXPECT_FLOAT_EQ(f[FT::kAge], 0.0F);
  for (std::size_t i = 0; i < FT::kNumDeltas; ++i) EXPECT_TRUE(std::isnan(f[FT::kDelta0 + i])) << i;
  EXPECT_FLOAT_EQ(f[FT::kCount], 1.0F);
  for (std::size_t i = 0; i < FT::kNumEdcs; ++i) EXPECT_FLOAT_EQ(f[FT::kEdc0 + i], 1.0F) << i;
}

TEST(FeatureTracker, ClockCountsAllKeysAccesses) {
  FT ft;
  EXPECT_EQ(ft.now(), 0U);
  ft.on_access(1, 1);  // t = 0
  EXPECT_EQ(ft.now(), 0U);
  tick(ft, 4);  // t = 1..4
  EXPECT_EQ(ft.now(), 4U);
  EXPECT_FLOAT_EQ(features(ft, 1)[FT::kAge], 4.0F);
}

TEST(FeatureTracker, DeltasMostRecentFirst) {
  FT ft;
  ft.on_access(1, 1);  // t = 0
  tick(ft, 2);
  ft.on_access(1, 1);  // t = 3, gap 3
  ft.on_access(1, 1);  // t = 4, gap 1
  tick(ft, 5);
  ft.on_access(1, 1);  // t = 10, gap 6
  const Row f = features(ft, 1);
  EXPECT_FLOAT_EQ(f[FT::kDelta0 + 0], 6.0F);
  EXPECT_FLOAT_EQ(f[FT::kDelta0 + 1], 1.0F);
  EXPECT_FLOAT_EQ(f[FT::kDelta0 + 2], 3.0F);
  for (std::size_t i = 3; i < FT::kNumDeltas; ++i) EXPECT_TRUE(std::isnan(f[FT::kDelta0 + i])) << i;
  EXPECT_FLOAT_EQ(f[FT::kCount], 4.0F);
  EXPECT_FLOAT_EQ(f[FT::kAge], 0.0F);
}

TEST(FeatureTracker, DeltaRingKeepsTheEightMostRecent) {
  // Gaps 1, 2, ..., 11: the ring wraps, and delta_1..8 must be 11, 10, ..., 4.
  FT ft;
  ft.on_access(1, 1);
  for (int gap = 1; gap <= 11; ++gap) {
    tick(ft, gap - 1);
    ft.on_access(1, 1);
  }
  const Row f = features(ft, 1);
  for (std::size_t i = 0; i < FT::kNumDeltas; ++i) {
    EXPECT_FLOAT_EQ(f[FT::kDelta0 + i], static_cast<float>(11 - i)) << i;
  }
  EXPECT_FLOAT_EQ(f[FT::kCount], 12.0F);
}

TEST(FeatureTracker, DecayedCountersUpdateAndDecay) {
  FT ft;
  ft.on_access(1, 1);  // t = 0, edc = 1
  tick(ft, 511);
  ft.on_access(1, 1);  // t = 512, gap 512: edc_i = 1 + 2^(-512/h_i)
  Row f = features(ft, 1);
  EXPECT_FLOAT_EQ(f[FT::kEdc0 + 0], 1.5F);  // h = 512
  EXPECT_FLOAT_EQ(f[FT::kEdc0 + 1], static_cast<float>(1.0 + std::exp2(-0.25)));
  EXPECT_FLOAT_EQ(f[FT::kEdc0 + 2], static_cast<float>(1.0 + std::exp2(-1.0 / 16)));
  EXPECT_FLOAT_EQ(f[FT::kEdc0 + 3], static_cast<float>(1.0 + std::exp2(-1.0 / 64)));

  // Reported values decay to now without a new access: one half-life of edc_1 halves it.
  tick(ft, 512);
  f = features(ft, 1);
  EXPECT_FLOAT_EQ(f[FT::kEdc0 + 0], 0.75F);
  EXPECT_FLOAT_EQ(f[FT::kAge], 512.0F);
}

TEST(FeatureTracker, SizeComesFromTheFirstAccess) {
  FT ft;
  ft.on_access(1, 8);
  ft.on_access(1, 4096);
  EXPECT_FLOAT_EQ(features(ft, 1)[FT::kLogSize], 3.0F);
}

TEST(FeatureTracker, SizeZeroCountsAsOne) {
  FT ft;
  ft.on_access(1, 0);
  EXPECT_FLOAT_EQ(features(ft, 1)[FT::kLogSize], 0.0F);
}

TEST(FeatureTracker, ForgetDropsHistory) {
  FT ft;
  ft.on_access(1, 1);
  ft.on_access(1, 1);
  ft.forget(1);
  ft.forget(42);  // unknown: no-op
  EXPECT_FALSE(ft.contains(1));
  EXPECT_EQ(ft.size(), 0U);
  EXPECT_EQ(ft.now(), 1U);  // forgetting doesn't rewind the clock

  ft.on_access(1, 1);  // starts over
  const Row f = features(ft, 1);
  EXPECT_FLOAT_EQ(f[FT::kCount], 1.0F);
  EXPECT_TRUE(std::isnan(f[FT::kDelta0]));
}

// ---- Memory window (history kept for forgotten keys) ------------------------------------------

TEST(FeatureTracker, ForgottenKeyContinuesItsHistoryWhenItComesBack) {
  FT ft(4);
  ft.on_access(1, 1);  // t = 0
  ft.on_access(1, 1);  // t = 1, gap 1
  ft.forget(1);
  EXPECT_FALSE(ft.contains(1));
  EXPECT_TRUE(ft.remembers(1));
  EXPECT_EQ(ft.size(), 0U);
  EXPECT_EQ(ft.ghost_count(), 1U);

  tick(ft, 5);         // t = 2..6
  ft.on_access(1, 1);  // t = 7, gap 6 (the time it spent evicted counts)
  EXPECT_TRUE(ft.contains(1));
  EXPECT_FALSE(ft.remembers(1));
  EXPECT_EQ(ft.ghost_count(), 0U);
  const Row f = features(ft, 1);
  EXPECT_FLOAT_EQ(f[FT::kCount], 3.0F);
  EXPECT_FLOAT_EQ(f[FT::kDelta0 + 0], 6.0F);
  EXPECT_FLOAT_EQ(f[FT::kDelta0 + 1], 1.0F);
}

TEST(FeatureTracker, OldestForgottenKeyIsDroppedFirst) {
  FT ft(2);
  for (const Key k : {Key{1}, Key{2}, Key{3}}) ft.on_access(k, 1);
  ft.forget(1);
  ft.forget(2);
  ft.forget(3);  // over capacity: 1 goes
  EXPECT_FALSE(ft.remembers(1));
  EXPECT_TRUE(ft.remembers(2));
  EXPECT_TRUE(ft.remembers(3));
  EXPECT_EQ(ft.ghost_count(), 2U);

  ft.on_access(1, 1);  // starts over
  EXPECT_FLOAT_EQ(features(ft, 1)[FT::kCount], 1.0F);
}

TEST(FeatureTracker, ComingBackLeavesTheWindowAndRejoinsAtTheEnd) {
  FT ft(2);
  for (const Key k : {Key{1}, Key{2}, Key{3}}) ft.on_access(k, 1);
  ft.forget(1);
  ft.on_access(1, 1);  // tracked again: no longer counts toward the window
  ft.forget(2);
  ft.forget(3);
  EXPECT_TRUE(ft.contains(1));
  EXPECT_EQ(ft.ghost_count(), 2U);
  ft.forget(1);  // now the newest ghost; the oldest (2) is dropped
  EXPECT_FALSE(ft.remembers(2));
  EXPECT_TRUE(ft.remembers(3));
  EXPECT_TRUE(ft.remembers(1));
}

TEST(FeatureTracker, ForgetIsIdempotentAndIgnoresUnknownKeys) {
  FT ft(4);
  ft.on_access(1, 1);
  ft.forget(1);
  ft.forget(1);   // already a ghost
  ft.forget(42);  // unknown
  EXPECT_EQ(ft.ghost_count(), 1U);
  EXPECT_EQ(ft.size(), 0U);
}

TEST(FeatureTracker, UnknownKeyThrows) {
  const FT ft;
  Row row{};
  EXPECT_THROW(ft.write_features(5, row), std::out_of_range);
}

}  // namespace
}  // namespace lcache
