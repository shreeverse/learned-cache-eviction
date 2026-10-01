#include "ml/predictor.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "ml/feature_tracker.h"

namespace lcache {
namespace {

constexpr std::size_t kF = FeatureTracker::kNumFeatures;

std::vector<double> run(Predictor& p, const std::vector<Key>& keys) {
  const std::vector<float> features(keys.size() * kF, 0.0F);
  std::vector<double> scores(keys.size(), -1.0);
  p.predict(keys, features, scores);
  return scores;
}

TEST(OraclePredictor, ScoresAreTheNextAccess) {
  OraclePredictor p;
  EXPECT_TRUE(p.needs_next_access());
  p.observe(Request{.key = 1, .next_vtime = 30});
  p.observe(Request{.key = 2, .next_vtime = kNever});
  p.observe(Request{.key = 3, .next_vtime = 5});
  p.observe(Request{.key = 1, .next_vtime = 40});  // a later request for key 1 replaces 30

  const auto s = run(p, {1, 2, 3});
  EXPECT_DOUBLE_EQ(s[0], 40.0);
  EXPECT_TRUE(std::isinf(s[1]) && s[1] > 0);
  EXPECT_DOUBLE_EQ(s[2], 5.0);
}

TEST(OraclePredictor, UnobservedKeyThrows) {
  OraclePredictor p;
  EXPECT_THROW(run(p, {9}), std::out_of_range);
}

TEST(RandomPredictor, InUnitIntervalAndDeterministic) {
  RandomPredictor a(7);
  RandomPredictor b(7);
  EXPECT_FALSE(a.needs_next_access());
  const std::vector<Key> keys{1, 2, 3, 4, 5, 6, 7, 8};
  const auto sa = run(a, keys);
  EXPECT_EQ(sa, run(b, keys));
  for (const double s : sa) {
    EXPECT_GE(s, 0.0);
    EXPECT_LT(s, 1.0);
  }
}

TEST(AgePredictor, ScoresAreTheAgeColumn) {
  AgePredictor p;
  const std::vector<Key> keys{4, 9};
  std::vector<float> features(2 * kF, 0.0F);
  features[FeatureTracker::kAge] = 3.0F;
  features[kF + FeatureTracker::kAge] = 17.0F;
  std::vector<double> scores(2);
  p.predict(keys, features, scores);
  EXPECT_DOUBLE_EQ(scores[0], 3.0);
  EXPECT_DOUBLE_EQ(scores[1], 17.0);
}

TEST(FunctionPredictor, PassesEachKeyWithItsOwnRow) {
  FunctionPredictor p([](Key k, std::span<const float> row) {
    EXPECT_EQ(row.size(), kF);
    return static_cast<double>(k) * 10.0 + row[FeatureTracker::kAge];
  });
  const std::vector<Key> keys{3, 5};
  std::vector<float> features(2 * kF, 0.0F);
  features[FeatureTracker::kAge] = 1.0F;       // row 0
  features[kF + FeatureTracker::kAge] = 2.0F;  // row 1
  std::vector<double> scores(2);
  p.predict(keys, features, scores);
  EXPECT_DOUBLE_EQ(scores[0], 31.0);
  EXPECT_DOUBLE_EQ(scores[1], 52.0);
  EXPECT_EQ(p.calls(), 1U);
  EXPECT_EQ(p.last_batch_size(), 2U);
}

}  // namespace
}  // namespace lcache
