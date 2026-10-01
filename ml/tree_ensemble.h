#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ml/predictor.h"

namespace lcache {

// Evaluates a LightGBM regression model ourselves, without the LightGBM library (M3 step 8,
// decisions D19). Reads LightGBM's text model format and reproduces its predictions exactly
// (same decisions, same tree order, same double arithmetic; tests check bit equality).
//
// Why: per-eviction latency (docs/m3-results.md, step 8). Each tree walk is a chain of dependent
// loads and unpredictable branches, so the evaluator (1) makes every step branchless: leaves are
// nodes that point to themselves, and each walk takes exactly the tree's depth in steps; and (2)
// walks kLanes rows through a tree together, so their independent chains overlap in the CPU.
// Trees are still summed in order for every row, so results stay bit-identical to LightGBM.
//
// Supported: numerical splits (no categorical features), no linear trees, objective=regression
// (identity output). Anything else is rejected at load, so it can never silently differ.
class TreeEnsemblePredictor final : public Predictor {
 public:
  // Throws std::runtime_error if the file can't be read, isn't a supported model, or its
  // feature names are neither FeatureTracker::feature_names(false) nor (true).
  explicit TreeEnsemblePredictor(const std::string& model_path);

  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;

  // Score of one feature row (FeatureTracker::kNumFeatures values).
  double predict_row(std::span<const float> row) const;

  std::size_t num_trees() const { return trees_.size(); }
  std::optional<bool> normalized_features() const override { return normalized_; }

  static constexpr std::size_t kLanes = 8;  // rows walked through a tree together

 private:
  template <bool kZeroMissing>
  void predict_lanes(std::span<const float> features, std::span<double> scores) const;

  // Parses one "Tree=" section of the model file and appends it.
  void add_tree(const std::string& path, std::size_t index,
                const std::map<std::string, std::string>& section);

  // A split or a leaf, 24 bytes. Children are node indices within the tree; a leaf's children
  // are itself, so extra steps past a leaf stay on it, and its value is stored in `threshold`.
  struct Node {
    double threshold = 0;                 // split threshold, or the leaf value
    std::array<std::int32_t, 2> child{};  // [0] = left, [1] = right: indexed, not branched on
    std::uint16_t feature = 0;
    std::uint8_t nan_left = 0;      // where NaN goes (precomputed from LightGBM's rules)
    std::uint8_t zero_missing = 0;  // LightGBM "zero" missing type: |v| <= 1e-35 goes...
    std::uint8_t default_left = 0;  // ...to the default side
  };
  static_assert(sizeof(Node) == 24);

  struct Tree {
    std::size_t offset = 0;  // first node (the root) in nodes_
    std::size_t depth = 0;   // steps from the root to the deepest leaf
  };

  std::vector<Tree> trees_;
  std::vector<Node> nodes_;
  bool any_zero_missing_ = false;
  bool normalized_ = false;  // if no split uses it, a cheaper step is used
};

}  // namespace lcache
