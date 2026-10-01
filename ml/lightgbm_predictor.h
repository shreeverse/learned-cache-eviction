#pragma once

#include <memory>
#include <span>
#include <string>

#include "ml/predictor.h"

namespace lcache {

// Scores candidates with a LightGBM model trained by tools/train.py (docs/m3-design.md section
// 2). The model predicts log1p(distance to next access), so a higher score is a better victim.
//
// One batched LGBM_BoosterPredictForMat call per eviction, single-threaded. The LightGBM C API
// is used only in the .cpp, so this header doesn't need LightGBM's include path.
class LightGbmPredictor final : public Predictor {
 public:
  // Loads a text model. Throws std::runtime_error if the file can't be loaded or if the model's
  // feature names differ from FeatureTracker::feature_names() (a training/inference mismatch).
  explicit LightGbmPredictor(const std::string& model_path);

  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;

  // Boosting iterations in the model (= trees, for regression).
  int num_iterations() const { return num_iterations_; }

 private:
  struct BoosterDeleter {
    void operator()(void* booster) const;  // LGBM_BoosterFree
  };
  // LightGBM's BoosterHandle. Freed automatically, also when the constructor throws.
  std::unique_ptr<void, BoosterDeleter> booster_;
  int num_iterations_ = 0;
};

}  // namespace lcache
