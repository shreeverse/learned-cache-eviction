#include "ml/tree_ensemble.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ml/feature_tracker.h"

namespace lcache {

namespace {

constexpr std::size_t kF = FeatureTracker::kNumFeatures;

// LightGBM's constants (include/LightGBM/meta.h, tree.h in v4.5.0).
constexpr double kZeroThreshold = 1e-35F;
constexpr int kCategoricalMask = 1;
constexpr int kDefaultLeftMask = 2;
constexpr std::uint8_t kMissingZero = 1;
constexpr std::uint8_t kMissingNaN = 2;

[[noreturn]] void fail(const std::string& path, const std::string& what) {
  throw std::runtime_error("model '" + path + "': " + what);
}

std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

// strtod is correctly rounded, like LightGBM's parser, so thresholds and leaf values are the
// same doubles LightGBM uses.
std::vector<double> parse_doubles(const std::string& s) {
  std::vector<double> out;
  for (const auto& w : split_words(s)) out.push_back(std::strtod(w.c_str(), nullptr));
  return out;
}

std::vector<int> parse_ints(const std::string& s) {
  std::vector<int> out;
  for (const auto& w : split_words(s)) {
    int v = 0;
    std::from_chars(w.data(), w.data() + w.size(), v);
    out.push_back(v);
  }
  return out;
}

using Section = std::map<std::string, std::string>;

// Splits the file into the header and one key=value section per tree.
void read_sections(const std::string& path, Section& header, std::vector<Section>& trees) {
  std::ifstream in(path);
  if (!in) fail(path, "cannot open");
  Section* current = &header;
  for (std::string line; std::getline(in, line);) {
    if (line == "end of trees") return;
    if (line.rfind("Tree=", 0) == 0) {
      trees.emplace_back();
      current = &trees.back();
      continue;
    }
    const auto eq = line.find('=');
    if (eq != std::string::npos) (*current)[line.substr(0, eq)] = line.substr(eq + 1);
  }
  fail(path, "no 'end of trees' line (not a LightGBM text model?)");
}

const std::string& field(const std::string& path, const Section& s, const std::string& key) {
  const auto it = s.find(key);
  if (it == s.end()) fail(path, "missing field '" + key + "'");
  return it->second;
}

// Rejects a split this evaluator can't reproduce exactly, or that would index out of range.
void check_split(const std::string& path, const std::string& where, int feature, int decision,
                 std::array<int, 2> children, int num_leaves) {
  if ((decision & kCategoricalMask) != 0) fail(path, where + "categorical split");
  if (feature < 0 || static_cast<std::size_t>(feature) >= kF) {
    fail(path, where + "split on an unknown feature");
  }
  for (const int c : children) {
    if (c >= num_leaves - 1 || ~c >= num_leaves) fail(path, where + "child out of range");
  }
}

// Steps from the root to the deepest leaf of a tree laid out as in add_tree (splits 0..n-1, then
// leaves). Returns n + 1 if a path is longer than n splits, i.e. the tree has a cycle.
template <typename Node>
std::size_t tree_depth(const Node* tree, std::size_t n) {
  std::size_t depth = 0;
  std::vector<std::pair<std::int32_t, std::size_t>> stack{{0, 0}};
  while (!stack.empty()) {
    const auto [node, d] = stack.back();
    stack.pop_back();
    if (static_cast<std::size_t>(node) >= n) {
      depth = std::max(depth, d);
    } else if (d >= n) {
      return n + 1;
    } else {
      for (const std::int32_t c : tree[node].child) stack.emplace_back(c, d + 1);
    }
  }
  return depth;
}

}  // namespace

TreeEnsemblePredictor::TreeEnsemblePredictor(const std::string& model_path) {
  Section header;
  std::vector<Section> trees;
  read_sections(model_path, header, trees);

  if (field(model_path, header, "objective") != "regression") {
    fail(model_path, "objective is '" + field(model_path, header, "objective") +
                         "'; only 'regression' is supported (use the LightGBM predictor)");
  }
  if (field(model_path, header, "num_tree_per_iteration") != "1") {
    fail(model_path, "multi-class models are not supported");
  }
  const auto names = split_words(field(model_path, header, "feature_names"));
  const auto matches = [&names](bool normalized) {
    const auto expected = FeatureTracker::feature_names(normalized);
    return names.size() == expected.size() &&
           std::equal(names.begin(), names.end(), expected.begin());
  };
  if (matches(false)) {
    normalized_ = false;
  } else if (matches(true)) {
    normalized_ = true;
  } else {
    fail(model_path, "feature names [" + field(model_path, header, "feature_names") +
                         "] are not FeatureTracker's (raw or normalized)");
  }
  if (trees.empty()) fail(model_path, "no trees");

  for (std::size_t t = 0; t < trees.size(); ++t) add_tree(model_path, t, trees[t]);
}

void TreeEnsemblePredictor::add_tree(const std::string& path, std::size_t index, const Section& s) {
  const std::string where = "tree " + std::to_string(index) + ": ";
  if (s.contains("num_cat") && field(path, s, "num_cat") != "0") {
    fail(path, where + "categorical splits are not supported");
  }
  if (s.contains("is_linear") && field(path, s, "is_linear") != "0") {
    fail(path, where + "linear trees are not supported");
  }
  const auto leaf_values = parse_doubles(field(path, s, "leaf_value"));
  const int num_leaves = parse_ints(field(path, s, "num_leaves")).at(0);
  if (num_leaves < 1 || leaf_values.size() != static_cast<std::size_t>(num_leaves)) {
    fail(path, where + "leaf count mismatch");
  }
  if (num_leaves == 1) {  // a constant tree: just a leaf, no split lines
    trees_.push_back(Tree{.offset = nodes_.size(), .depth = 0});
    nodes_.push_back(Node{.threshold = leaf_values[0]});
    return;
  }
  const auto feature = parse_ints(field(path, s, "split_feature"));
  const auto threshold = parse_doubles(field(path, s, "threshold"));
  const auto decision = parse_ints(field(path, s, "decision_type"));
  const auto left = parse_ints(field(path, s, "left_child"));
  const auto right = parse_ints(field(path, s, "right_child"));
  const auto n = static_cast<std::size_t>(num_leaves - 1);
  if (feature.size() != n || threshold.size() != n || decision.size() != n || left.size() != n ||
      right.size() != n) {
    fail(path, where + "split arrays have the wrong length");
  }

  // Layout within the tree: splits 0..n-1 (LightGBM's order, root first), then leaf i at n + i.
  // LightGBM writes a leaf child as ~leaf.
  const auto child = [n](int c) {
    return static_cast<std::int32_t>(c >= 0 ? c : static_cast<int>(n) + ~c);
  };
  const std::size_t offset = nodes_.size();
  for (std::size_t i = 0; i < n; ++i) {
    check_split(path, where, feature[i], decision[i], {left[i], right[i]}, num_leaves);
    // LightGBM's Tree::NumericalDecision, resolved per split for NaN: if NaN is the "missing"
    // value it goes to the default side; otherwise it is treated as 0.0, which then goes to the
    // default side under the "zero" missing type, or is compared with the threshold.
    const bool default_left = (decision[i] & kDefaultLeftMask) != 0;
    const auto missing_type = static_cast<std::uint8_t>((decision[i] >> 2) & 3);
    bool nan_left = 0.0 <= threshold[i];
    if (missing_type == kMissingNaN || missing_type == kMissingZero) nan_left = default_left;
    any_zero_missing_ = any_zero_missing_ || missing_type == kMissingZero;
    nodes_.push_back(Node{.threshold = threshold[i],
                          .child = {child(left[i]), child(right[i])},
                          .feature = static_cast<std::uint16_t>(feature[i]),
                          .nan_left = static_cast<std::uint8_t>(nan_left ? 1 : 0),
                          .zero_missing = static_cast<std::uint8_t>(missing_type == kMissingZero),
                          .default_left = static_cast<std::uint8_t>(default_left ? 1 : 0)});
  }
  for (std::size_t i = 0; i < leaf_values.size(); ++i) {
    const auto self = static_cast<std::int32_t>(n + i);
    nodes_.push_back(Node{.threshold = leaf_values[i], .child = {self, self}});
  }

  const std::size_t depth = tree_depth(nodes_.data() + offset, n);
  if (depth > n) fail(path, where + "cycle in the tree");
  trees_.push_back(Tree{.offset = offset, .depth = depth});
}

namespace {

// One branchless step down the tree; equivalent to LightGBM's Tree::NumericalDecision (see
// add_tree). kZeroMissing = false skips the rare "zero is missing" rule when no split uses it.
template <bool kZeroMissing, typename Node>
std::int32_t step(const Node& nd, const double* x) {
  const double v = x[nd.feature];
  // Bitwise, not short-circuit, so the compiler emits no branches: tree data makes every
  // branch here close to a coin flip.
  const unsigned nan = std::isnan(v) ? 1U : 0U;
  const unsigned le = v <= nd.threshold ? 1U : 0U;  // false for NaN
  unsigned go_left = (nan & nd.nan_left) | (~nan & le & 1U);
  if constexpr (kZeroMissing) {
    const unsigned zero =
        nd.zero_missing & ((v >= -kZeroThreshold) ? 1U : 0U) & ((v <= kZeroThreshold) ? 1U : 0U);
    go_left = (zero & nd.default_left) | (~zero & go_left & 1U);
  }
  return nd.child[go_left ^ 1U];
}

}  // namespace

double TreeEnsemblePredictor::predict_row(std::span<const float> row) const {
  assert(row.size() == kF);
  std::array<double, kF> x{};
  for (std::size_t j = 0; j < kF; ++j) x[j] = row[j];  // LightGBM also predicts on doubles
  double sum = 0.0;
  for (const Tree& t : trees_) {
    const Node* tree = nodes_.data() + t.offset;
    std::int32_t node = 0;
    for (std::size_t d = 0; d < t.depth; ++d) {
      node =
          any_zero_missing_ ? step<true>(tree[node], x.data()) : step<false>(tree[node], x.data());
    }
    sum += tree[node].threshold;  // a leaf's value
  }
  return sum;
}

template <bool kZeroMissing>
void TreeEnsemblePredictor::predict_lanes(std::span<const float> features,
                                          std::span<double> scores) const {
  const std::size_t rows = scores.size();
  for (std::size_t base = 0; base < rows; base += kLanes) {
    const std::size_t lanes = std::min(kLanes, rows - base);
    std::array<std::array<double, kF>, kLanes> x{};
    for (std::size_t l = 0; l < lanes; ++l) {
      for (std::size_t j = 0; j < kF; ++j) x[l][j] = features[(base + l) * kF + j];
    }
    std::array<double, kLanes> sum{};
    for (const Tree& t : trees_) {
      const Node* tree = nodes_.data() + t.offset;
      std::array<std::int32_t, kLanes> node{};
      for (std::size_t d = 0; d < t.depth; ++d) {
        // Independent walks: the CPU overlaps their loads instead of waiting on each in turn.
        for (std::size_t l = 0; l < kLanes; ++l) {
          node[l] = step<kZeroMissing>(tree[node[l]], x[l].data());
        }
      }
      for (std::size_t l = 0; l < kLanes; ++l) sum[l] += tree[node[l]].threshold;
    }
    for (std::size_t l = 0; l < lanes; ++l) scores[base + l] = sum[l];
  }
}

void TreeEnsemblePredictor::predict([[maybe_unused]] std::span<const Key> keys,
                                    std::span<const float> features, std::span<double> scores) {
  assert(features.size() == keys.size() * kF && scores.size() == keys.size());
  if (any_zero_missing_) {
    predict_lanes<true>(features, scores);
  } else {
    predict_lanes<false>(features, scores);
  }
}

}  // namespace lcache
