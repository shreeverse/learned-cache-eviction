#include "ml/lightgbm_predictor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ml/feature_tracker.h"
#include "ml/tree_ensemble.h"
#include "policy/policy_factory.h"
#include "reference/workloads.h"
#include "sim/simulator.h"
#include "test_util.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

constexpr std::size_t kF = FeatureTracker::kNumFeatures;

// Written by tools/make_test_model.py.
const std::string kModel = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";
const std::string kCases = std::string(LC_TEST_DATA_DIR) + "/tiny_model_cases.csv";

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Cases {
  std::vector<float> features;  // row-major
  std::vector<double> expected;
};

Cases load_cases() {
  std::ifstream in(kCases);
  std::string line;
  std::getline(in, line);  // header
  Cases c;
  while (std::getline(in, line)) {
    std::stringstream ss(line);
    std::string cell;
    for (std::size_t j = 0; j < kF; ++j) {
      std::getline(ss, cell, ',');
      c.features.push_back(std::stof(cell));  // parses "nan" too
    }
    std::getline(ss, cell, ',');
    c.expected.push_back(std::stod(cell));
  }
  return c;
}

TEST(LightGbmPredictor, ReproducesPythonPredictions) {
  LightGbmPredictor p(kModel);
  EXPECT_EQ(p.num_iterations(), 4);
  EXPECT_FALSE(p.needs_next_access());

  const Cases c = load_cases();
  ASSERT_EQ(c.expected.size(), 16U);
  const std::vector<Key> keys(c.expected.size(), 0);  // the model ignores keys
  std::vector<double> scores(c.expected.size(), -1.0);
  p.predict(keys, c.features, scores);
  for (std::size_t i = 0; i < scores.size(); ++i) {
    EXPECT_NEAR(scores[i], c.expected[i], 1e-12) << "row " << i;
  }
}

TEST(LightGbmPredictor, BatchAndSingleRowsAgree) {
  LightGbmPredictor p(kModel);
  const Cases c = load_cases();
  for (std::size_t i = 0; i < c.expected.size(); ++i) {
    const std::vector<Key> one{0};
    std::vector<double> score(1);
    p.predict(one, std::span(c.features).subspan(i * kF, kF), score);
    EXPECT_NEAR(score[0], c.expected[i], 1e-12) << "row " << i;
  }
}

TEST(LightGbmPredictor, MissingFileThrows) {
  try {
    const LightGbmPredictor p("/nonexistent/model.txt");
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("/nonexistent/model.txt"), std::string::npos) << e.what();
  }
}

TEST(LightGbmPredictor, FeatureNameMismatchThrows) {
  // Same model with one feature renamed, as if trained against a different FeatureTracker.
  std::string text = read_file(kModel);
  const std::string from = "feature_names=log_size age ";
  const auto pos = text.find(from);
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, from.size(), "feature_names=log_size age_seconds ");
  const test::TempFile file(".txt");
  file.write(text);

  try {
    const LightGbmPredictor p(file.path().string());
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("age_seconds"), std::string::npos) << e.what();
  }
}

// The compiled evaluator (TreeEnsemblePredictor) must give bit-identical scores to LightGBM on
// the real models, so every hit rate measured with LightGBM still holds.
TEST(LightGbmPredictor, CompiledTreesMatchLightGbmBitForBitOnTheRealModels) {
  std::mt19937_64 rng(3);
  std::vector<float> rows;
  constexpr std::size_t kRows = 20'000;
  for (std::size_t i = 0; i < kRows; ++i) {
    std::array<float, kF> r{};
    r[FeatureTracker::kLogSize] = std::uniform_real_distribution<float>(0, 18)(rng);
    r[FeatureTracker::kAge] = static_cast<float>(rng() % 2'000'000);
    const auto history = rng() % 10;  // 0..9 accesses' worth of gaps
    for (std::size_t j = 0; j < FeatureTracker::kNumDeltas; ++j) {
      r[FeatureTracker::kDelta0 + j] = j < history ? static_cast<float>(1 + rng() % 60'000)
                                                   : std::numeric_limits<float>::quiet_NaN();
    }
    r[FeatureTracker::kCount] = static_cast<float>(1 + history);
    for (std::size_t j = 0; j < FeatureTracker::kNumEdcs; ++j) {
      r[FeatureTracker::kEdc0 + j] = std::uniform_real_distribution<float>(0, 8)(rng);
    }
    if (i % 7 == 0) r[rng() % kF] = 0.0F;  // exact zeros hit the "missing = zero" rule
    rows.insert(rows.end(), r.begin(), r.end());
  }
  const std::vector<Key> keys(kRows, 0);
  for (const char* name : {"twitter26_1MiB_h10k_32x31.txt", "twitter26_1MiB_h10k_128x63.txt"}) {
    const std::string path = std::string(LC_MODELS_DIR) + "/" + name;
    LightGbmPredictor lgbm(path);
    TreeEnsemblePredictor compiled(path);
    std::vector<double> want(kRows);
    std::vector<double> got(kRows);
    lgbm.predict(keys, rows, want);
    compiled.predict(keys, rows, got);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < kRows; ++i) {
      if (want[i] != got[i]) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0U) << name;
  }
}

TEST(LightGbmPredictor, LearnedPolicyThroughTheFactory) {
  EXPECT_TRUE(has_lightgbm());
  EXPECT_THROW(make_policy("learned", 100), std::invalid_argument);  // no model path

  const auto keys = test::zipf_keys(5'000, 300, 0.8, 5);
  std::vector<Request> reqs;
  for (const Key k : keys) reqs.push_back(test::req(k));
  VectorReader reader(std::move(reqs));
  // Both predictors give identical scores, so the same hits.
  std::uint64_t hits[2] = {0, 0};
  int i = 0;
  for (const char* predictor : {"compiled", "lightgbm"}) {
    const auto policy = make_policy(
        "learned",
        PolicyConfig{.capacity_bytes = 50, .model_path = kModel, .predictor = predictor});
    EXPECT_EQ(policy->name(), "learned");
    EXPECT_FALSE(policy->needs_next_access());
    const auto r = simulate(reader, *policy, {.capacity_bytes = 50});
    EXPECT_EQ(r.requests, 5'000U);
    hits[i++] = r.hits;
  }
  EXPECT_GT(hits[0], 0U);
  EXPECT_EQ(hits[0], hits[1]);
  EXPECT_THROW(
      make_policy("learned",
                  PolicyConfig{.capacity_bytes = 50, .model_path = kModel, .predictor = "nope"}),
      std::invalid_argument);
}

}  // namespace
}  // namespace lcache
