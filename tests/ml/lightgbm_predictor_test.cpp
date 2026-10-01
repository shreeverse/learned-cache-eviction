#include "ml/lightgbm_predictor.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ml/feature_tracker.h"
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

TEST(LightGbmPredictor, LearnedPolicyThroughTheFactory) {
  EXPECT_TRUE(has_learned_policy());
  EXPECT_THROW(make_policy("learned", 100), std::invalid_argument);  // no model path

  const auto keys = test::zipf_keys(5'000, 300, 0.8, 5);
  std::vector<Request> reqs;
  for (const Key k : keys) reqs.push_back(test::req(k));
  VectorReader reader(std::move(reqs));
  const auto policy =
      make_policy("learned", PolicyConfig{.capacity_bytes = 50, .model_path = kModel});
  EXPECT_EQ(policy->name(), "learned");
  EXPECT_FALSE(policy->needs_next_access());
  const auto r = simulate(reader, *policy, {.capacity_bytes = 50});
  EXPECT_EQ(r.requests, 5'000U);
  EXPECT_GT(r.hits, 0U);
}

}  // namespace
}  // namespace lcache
