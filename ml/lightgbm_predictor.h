#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ml/predictor.h"

namespace lcache {

// Scores candidates with a LightGBM model trained by tools/train.py (docs/m3-design.md section
// 2). The model predicts log1p(distance to next access), so a higher score is a better victim.
//
// Uses LightGBM's "fast single-row" API: the prediction config is parsed once at load, and each
// candidate is one call. Measured on 1M Twitter requests at 1 MiB: 63.4 s vs 73.1 s with one
// batched LGBM_BoosterPredictForMat per eviction. Most of the remaining time is LightGBM's own
// per-row cost (2.77 us/row for 32 trees x 31 leaves, measured in Python), not call overhead.
// Not thread-safe: the fast config holds per-call buffers. The LightGBM C API is used only in
// the .cpp, so this header doesn't need LightGBM's include path.
class LightGbmPredictor final : public Predictor {
 public:
  // Loads a text model. Throws std::runtime_error if the file can't be loaded or if the model's
  // feature names differ from FeatureTracker::feature_names() (a training/inference mismatch).
  explicit LightGbmPredictor(const std::string& model_path);

  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;

  // Boosting iterations in the model (= trees, for regression).
  int num_iterations() const { return num_iterations_; }
  std::optional<bool> normalized_features() const override { return normalized_; }

 private:
  struct BoosterDeleter {
    void operator()(void* booster) const;  // LGBM_BoosterFree
  };
  struct FastConfigDeleter {
    void operator()(void* config) const;  // LGBM_FastConfigFree
  };
  // LightGBM's BoosterHandle and FastConfigHandle. Freed automatically, also when the
  // constructor throws; the config is declared last so it is freed before the booster.
  std::unique_ptr<void, BoosterDeleter> booster_;
  std::unique_ptr<void, FastConfigDeleter> fast_config_;
  int num_iterations_ = 0;
  bool normalized_ = false;
};

}  // namespace lcache
