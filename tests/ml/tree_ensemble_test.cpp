#include "ml/tree_ensemble.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ml/feature_tracker.h"
#include "test_util.h"

namespace lcache {
namespace {

constexpr std::size_t kF = FeatureTracker::kNumFeatures;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
const std::string kTinyModel = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";
const std::string kTinyCases = std::string(LC_TEST_DATA_DIR) + "/tiny_model_cases.csv";

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string header(const std::string& objective = "regression") {
  std::string names;
  for (const auto n : FeatureTracker::feature_names())
    names += (names.empty() ? "" : " ") + std::string(n);
  return "tree\nversion=v4\nnum_class=1\nnum_tree_per_iteration=1\nlabel_index=0\n"
         "max_feature_idx=14\nobjective=" +
         objective + "\nfeature_names=" + names + "\n\n";
}

// Three trees, each exercising one LightGBM decision rule (decision_type = default_left bit 2 |
// missing type << 2):
//   tree 0: a single leaf, 0.5
//   tree 1: age <= 10 ? 1 : 2; NaN goes left (decision_type 2|8 = 10)
//   tree 2: count <= 3 ? 10 : 20; missing type "zero", zeros go right (decision_type 1<<2 = 4)
//   tree 3: log_size <= 5 ? 100 : 200; no missing type: NaN is treated as 0 (decision_type 0)
std::string handmade_model() {
  return header() +
         "Tree=0\nnum_leaves=1\nnum_cat=0\nleaf_value=0.5\nshrinkage=1\n\n"
         "Tree=1\nnum_leaves=2\nnum_cat=0\nsplit_feature=1\nthreshold=10\ndecision_type=10\n"
         "left_child=-1\nright_child=-2\nleaf_value=1 2\nis_linear=0\nshrinkage=1\n\n"
         "Tree=2\nnum_leaves=2\nnum_cat=0\nsplit_feature=10\nthreshold=3\ndecision_type=4\n"
         "left_child=-1\nright_child=-2\nleaf_value=10 20\nis_linear=0\nshrinkage=1\n\n"
         "Tree=3\nnum_leaves=2\nnum_cat=0\nsplit_feature=0\nthreshold=5\ndecision_type=0\n"
         "left_child=-1\nright_child=-2\nleaf_value=100 200\nis_linear=0\nshrinkage=1\n\n"
         "end of trees\n";
}

std::array<float, kF> row(float log_size, float age, float count) {
  std::array<float, kF> r{};
  r.fill(1.0F);
  r[FeatureTracker::kLogSize] = log_size;
  r[FeatureTracker::kAge] = age;
  r[FeatureTracker::kCount] = count;
  return r;
}

TEST(TreeEnsemblePredictor, DecisionRulesMatchLightGbm) {
  const test::TempFile file(".txt");
  file.write(handmade_model());
  const TreeEnsemblePredictor p(file.path().string());
  EXPECT_EQ(p.num_trees(), 4U);

  // 0.5 + tree1 + tree2 + tree3
  EXPECT_EQ(p.predict_row(row(1, 5, 2)), 0.5 + 1 + 10 + 100);
  EXPECT_EQ(p.predict_row(row(9, 50, 7)), 0.5 + 2 + 20 + 200);
  EXPECT_EQ(p.predict_row(row(5, 10, 3)), 0.5 + 1 + 10 + 100);  // equal to threshold: left
  // NaN age: tree 1 has missing type NaN with default left.
  EXPECT_EQ(p.predict_row(row(1, kNaN, 2)), 0.5 + 1 + 10 + 100);
  // count 0 and count NaN: tree 2 has missing type zero, default right; NaN counts as zero.
  EXPECT_EQ(p.predict_row(row(1, 5, 0)), 0.5 + 1 + 20 + 100);
  EXPECT_EQ(p.predict_row(row(1, 5, kNaN)), 0.5 + 1 + 20 + 100);
  // NaN log_size: tree 3 has no missing type, so NaN becomes 0 and 0 <= 5 goes left.
  EXPECT_EQ(p.predict_row(row(kNaN, 5, 2)), 0.5 + 1 + 10 + 100);
}

TEST(TreeEnsemblePredictor, ReproducesPythonPredictionsExactly) {
  const TreeEnsemblePredictor p(kTinyModel);
  std::ifstream in(kTinyCases);
  std::string line;
  std::getline(in, line);  // header
  int rows = 0;
  while (std::getline(in, line)) {
    std::stringstream ss(line);
    std::string cell;
    std::array<float, kF> r{};
    for (std::size_t j = 0; j < kF; ++j) {
      std::getline(ss, cell, ',');
      r[j] = std::stof(cell);
    }
    std::getline(ss, cell, ',');
    EXPECT_EQ(p.predict_row(r), std::stod(cell)) << "row " << rows;  // bit-exact
    ++rows;
  }
  EXPECT_EQ(rows, 16);
}

TEST(TreeEnsemblePredictor, BatchUsesOneRowPerKey) {
  const TreeEnsemblePredictor p(kTinyModel);
  std::vector<float> features;
  for (const auto& r : {row(4, 100, 1), row(11, 40'000, 9)})
    features.insert(features.end(), r.begin(), r.end());
  const std::vector<Key> keys{1, 2};
  std::vector<double> scores(2);
  TreeEnsemblePredictor q(kTinyModel);
  q.predict(keys, features, scores);
  EXPECT_EQ(scores[0], p.predict_row(row(4, 100, 1)));
  EXPECT_EQ(scores[1], p.predict_row(row(11, 40'000, 9)));
}

void expect_rejected(const std::string& text, const std::string& reason) {
  const test::TempFile file(".txt");
  file.write(text);
  try {
    const TreeEnsemblePredictor p(file.path().string());
    FAIL() << "expected rejection: " << reason;
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(reason), std::string::npos) << e.what();
  }
}

TEST(TreeEnsemblePredictor, ReportsWhichFeatureSetTheModelWasTrainedOn) {
  EXPECT_EQ(TreeEnsemblePredictor(kTinyModel).normalized_features(), false);

  // The same model with the normalized names (as if trained on --normalize data).
  std::string text = read_file(kTinyModel);
  std::string norm_names;
  for (const auto n : FeatureTracker::feature_names(true)) {
    norm_names += (norm_names.empty() ? "" : " ") + std::string(n);
  }
  const auto line = text.find("feature_names=");
  text.replace(line, text.find('\n', line) - line, "feature_names=" + norm_names);
  const test::TempFile file(".txt");
  file.write(text);
  EXPECT_EQ(TreeEnsemblePredictor(file.path().string()).normalized_features(), true);
}

TEST(TreeEnsemblePredictor, RejectsWhatItCannotEvaluateExactly) {
  EXPECT_THROW(TreeEnsemblePredictor("/nonexistent/model.txt"), std::runtime_error);

  std::string binary = handmade_model();
  binary.replace(binary.find("objective=regression"), 20, "objective=binary sigmoid:1");
  expect_rejected(binary, "only 'regression'");

  std::string renamed = read_file(kTinyModel);
  renamed.replace(renamed.find("log_size age "), 13, "log_size agez ");
  expect_rejected(renamed, "not FeatureTracker's");

  std::string categorical = handmade_model();
  categorical.replace(categorical.find("decision_type=10"), 16, "decision_type=11");
  expect_rejected(categorical, "categorical");

  std::string linear = handmade_model();
  linear.replace(linear.find("is_linear=0"), 11, "is_linear=1");
  expect_rejected(linear, "linear");

  std::string truncated = handmade_model();
  truncated.erase(truncated.find("end of trees"));
  expect_rejected(truncated, "end of trees");
}

}  // namespace
}  // namespace lcache
