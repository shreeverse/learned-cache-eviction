#include "policy/learned.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "policy/belady.h"
#include "policy/lru.h"
#include "reference/workloads.h"
#include "sim/simulator.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

using FT = FeatureTracker;

// A LearnedPolicy over a FunctionPredictor, with a raw pointer to the predictor for inspection.
struct Fixture {
  FunctionPredictor* predictor = nullptr;
  std::unique_ptr<LearnedPolicy> policy;
};

Fixture make(FunctionPredictor::ScoreFn fn, LearnedOptions options = {}) {
  auto p = std::make_unique<FunctionPredictor>(std::move(fn));
  Fixture f{.predictor = p.get(), .policy = nullptr};
  f.policy = std::make_unique<LearnedPolicy>(std::move(p), "test", options);
  return f;
}

// Scores from a fixed table: key -> score (missing keys score 0).
FunctionPredictor::ScoreFn table(std::map<Key, double> scores) {
  return [scores = std::move(scores)](Key k, std::span<const float>) {
    const auto it = scores.find(k);
    return it == scores.end() ? 0.0 : it->second;
  };
}

// Scores by age: the least recently used candidate wins.
double by_age(Key /*k*/, std::span<const float> row) { return row[FT::kAge]; }

std::vector<Request> unit_requests(const std::vector<Key>& keys) {
  std::vector<Request> reqs;
  reqs.reserve(keys.size());
  for (const Key k : keys) reqs.push_back(test::req(k));
  return reqs;
}

TEST(LearnedPolicy, RejectsBadConstruction) {
  EXPECT_THROW(LearnedPolicy(nullptr, "x"), std::invalid_argument);
  EXPECT_THROW(LearnedPolicy(std::make_unique<RandomPredictor>(1), "x", {.sample_size = 0}),
               std::invalid_argument);
}

TEST(LearnedPolicy, NameAndFutureKnowledgeComeFromThePredictor) {
  const LearnedPolicy oracle(std::make_unique<OraclePredictor>(), "oracle-sampled");
  EXPECT_EQ(oracle.name(), "oracle-sampled");
  EXPECT_TRUE(oracle.needs_next_access());
  const LearnedPolicy random(std::make_unique<RandomPredictor>(1), "random-sampled");
  EXPECT_FALSE(random.needs_next_access());
}

TEST(LearnedPolicy, EvictsTheHighestScore) {
  auto f = make(table({{1, 5.0}, {2, 9.0}, {3, 1.0}}));
  for (const Key k : std::initializer_list<Key>{1, 2, 3}) f.policy->on_insert(test::req(k));
  EXPECT_EQ(f.policy->evict(), 2U);
  EXPECT_EQ(f.policy->evict(), 1U);
  EXPECT_EQ(f.policy->evict(), 3U);
}

TEST(LearnedPolicy, OnePredictCallPerEvictionWithEveryKeyWhenFewAreCached) {
  auto f = make(table({}), {.sample_size = 4});
  for (const Key k : std::initializer_list<Key>{1, 2, 3}) f.policy->on_insert(test::req(k));
  f.policy->evict();
  EXPECT_EQ(f.predictor->calls(), 1U);
  EXPECT_EQ(f.predictor->last_batch_size(), 3U);
}

TEST(LearnedPolicy, SamplesExactlySampleSizeCandidatesFromCachedKeys) {
  std::set<Key> seen;
  auto f = make(
      [&seen](Key k, std::span<const float>) {
        seen.insert(k);
        return 0.0;
      },
      {.sample_size = 4});
  for (Key k = 0; k < 100; ++k) f.policy->on_insert(test::req(k));
  f.policy->on_remove(50);
  for (int i = 0; i < 50; ++i) {
    f.policy->evict();
    EXPECT_EQ(f.predictor->last_batch_size(), 4U);
  }
  EXPECT_FALSE(seen.contains(50));  // removed keys are never candidates
  for (const Key k : seen) EXPECT_LT(k, 100U);
  EXPECT_GT(seen.size(), 50U);  // sampling spreads over the keys, not just a few
}

TEST(LearnedPolicy, FeaturesReachThePredictorRowByRow) {
  // Scoring by age makes it LRU over the candidates: key 2 is now the least recently used.
  auto f = make(by_age);
  for (const Key k : std::initializer_list<Key>{1, 2, 3}) f.policy->on_insert(test::req(k));
  f.policy->on_hit(test::req(1));
  EXPECT_EQ(f.policy->evict(), 2U);
  EXPECT_EQ(f.policy->evict(), 3U);
  EXPECT_EQ(f.policy->evict(), 1U);
}

TEST(LearnedPolicy, TiesGoToTheSmallestKey) {
  auto f = make(table({}));
  for (const Key k : std::initializer_list<Key>{7, 3, 9}) f.policy->on_insert(test::req(k));
  EXPECT_EQ(f.policy->evict(), 3U);
  EXPECT_EQ(f.policy->evict(), 7U);
}

TEST(LearnedPolicy, NanScoresNeverBeatANumber) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto f = make(table({{1, nan}, {2, -1e300}, {3, nan}}));
  for (const Key k : std::initializer_list<Key>{1, 2, 3}) f.policy->on_insert(test::req(k));
  EXPECT_EQ(f.policy->evict(), 2U);
}

TEST(LearnedPolicy, TheNewcomerCanBeTheVictim) {
  auto f = make(table({{4, 100.0}}));
  for (const Key k : std::initializer_list<Key>{1, 2, 3, 4}) f.policy->on_insert(test::req(k));
  EXPECT_EQ(f.policy->evict(), 4U);
}

TEST(LearnedPolicy, EvictionAndRemovalDropFeatureHistory) {
  auto f = make(table({{1, 10.0}}));
  for (const Key k : std::initializer_list<Key>{1, 2, 3}) f.policy->on_insert(test::req(k));
  f.policy->on_hit(test::req(1));
  EXPECT_EQ(f.policy->evict(), 1U);
  f.policy->on_remove(2);
  f.policy->on_remove(42);  // unknown: no-op
  EXPECT_FALSE(f.policy->tracker().contains(1));
  EXPECT_FALSE(f.policy->tracker().contains(2));
  EXPECT_TRUE(f.policy->tracker().contains(3));
  EXPECT_EQ(f.policy->evict(), 3U);

  f.policy->on_insert(test::req(1));  // comes back with a fresh history
  std::array<float, FT::kNumFeatures> row{};
  f.policy->tracker().write_features(1, row);
  EXPECT_FLOAT_EQ(row[FT::kCount], 1.0F);
}

TEST(LearnedPolicy, MemoryWindowKeepsHistoryOfEvictedKeys) {
  auto f = make(table({{1, 10.0}}), {.sample_size = 64, .seed = 1, .features = {.history = 8}});
  for (const Key k : std::initializer_list<Key>{1, 2}) f.policy->on_insert(test::req(k));
  f.policy->on_hit(test::req(1));
  EXPECT_EQ(f.policy->evict(), 1U);
  EXPECT_TRUE(f.policy->tracker().remembers(1));

  f.policy->on_insert(test::req(1));  // re-inserted: continues with count 3
  std::array<float, FT::kNumFeatures> row{};
  f.policy->tracker().write_features(1, row);
  EXPECT_FLOAT_EQ(row[FT::kCount], 3.0F);
  EXPECT_EQ(f.policy->evict(), 1U);  // still the only key the table scores high
}

// A predictor standing in for a model trained on normalized features.
class NormalizedModel final : public Predictor {
 public:
  void predict(std::span<const Key>, std::span<const float>, std::span<double> scores) override {
    for (double& s : scores) s = 0;
  }
  std::optional<bool> normalized_features() const override { return true; }
};

TEST(LearnedPolicy, RefusesAModelTrainedOnTheOtherFeatureKind) {
  EXPECT_THROW(LearnedPolicy(std::make_unique<NormalizedModel>(), "x"), std::invalid_argument);
  EXPECT_NO_THROW(
      LearnedPolicy(std::make_unique<NormalizedModel>(), "x", {.features = {.normalize = true}}));
}

TEST(LearnedPolicy, SameSeedSameEvictions) {
  const auto run = [](std::uint64_t seed) {
    LearnedPolicy p(std::make_unique<RandomPredictor>(seed), "r", {.sample_size = 8, .seed = seed});
    for (Key k = 0; k < 200; ++k) p.on_insert(test::req(k));
    std::vector<Key> victims;
    for (int i = 0; i < 100; ++i) victims.push_back(p.evict());
    return victims;
  };
  EXPECT_EQ(run(3), run(3));
  EXPECT_NE(run(3), run(4));
}

// ---- Through the simulator -------------------------------------------------------------------

TEST(LearnedPolicy, AgeScoresWithAllKeysAsCandidatesIsExactlyLru) {
  // Capacity 50 < sample size 64, so every cached key is a candidate and argmax age is the LRU key.
  const auto keys = test::zipf_keys(20'000, 500, 0.8, 11);
  VectorReader reader(unit_requests(keys));
  LruPolicy lru;
  LearnedPolicy learned(std::make_unique<FunctionPredictor>(by_age), "age");
  const auto expected = simulate(reader, lru, {.capacity_bytes = 50}).hits;
  EXPECT_EQ(simulate(reader, learned, {.capacity_bytes = 50}).hits, expected);
}

TEST(LearnedPolicy, OracleWithAllKeysAsCandidatesIsExactlyBelady) {
  // Ties only happen between keys never requested again, so tie-breaking can't change hits.
  const auto keys = test::zipf_keys(20'000, 500, 0.8, 12);
  VectorReader reader(unit_requests(keys));
  BeladyPolicy belady;
  LearnedPolicy oracle(std::make_unique<OraclePredictor>(), "oracle-sampled");
  const auto expected = simulate(reader, belady, {.capacity_bytes = 50}).hits;
  EXPECT_EQ(simulate(reader, oracle, {.capacity_bytes = 50}).hits, expected);
}

TEST(LearnedPolicy, SampledOracleIsBetweenRandomAndBelady) {
  const auto keys = test::zipf_keys(50'000, 5'000, 0.9, 13);
  VectorReader reader(unit_requests(keys));
  constexpr std::uint64_t kCap = 500;  // much more than the sample size, so it really samples
  BeladyPolicy belady;
  LearnedPolicy oracle(std::make_unique<OraclePredictor>(), "o", {.sample_size = 16});
  LearnedPolicy random(std::make_unique<RandomPredictor>(1), "r", {.sample_size = 16});
  const auto best = simulate(reader, belady, {.capacity_bytes = kCap}).hits;
  const auto sampled = simulate(reader, oracle, {.capacity_bytes = kCap}).hits;
  const auto worst = simulate(reader, random, {.capacity_bytes = kCap}).hits;
  EXPECT_LE(sampled, best);
  EXPECT_GT(sampled, worst);
}

}  // namespace
}  // namespace lcache
