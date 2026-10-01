#include "ml/predictor.h"

#include <cassert>
#include <limits>

#include "ml/feature_tracker.h"

namespace lcache {

void OraclePredictor::predict(std::span<const Key> keys, std::span<const float> /*features*/,
                              std::span<double> scores) {
  assert(scores.size() == keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const std::uint64_t next = next_.at(keys[i]);
    scores[i] =
        next == kNever ? std::numeric_limits<double>::infinity() : static_cast<double>(next);
  }
}

void RandomPredictor::predict([[maybe_unused]] std::span<const Key> keys,
                              std::span<const float> /*features*/, std::span<double> scores) {
  assert(scores.size() == keys.size());
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  for (double& s : scores) s = dist(rng_);
}

void FunctionPredictor::predict(std::span<const Key> keys, std::span<const float> features,
                                std::span<double> scores) {
  constexpr std::size_t kF = FeatureTracker::kNumFeatures;
  assert(scores.size() == keys.size() && features.size() == keys.size() * kF);
  ++calls_;
  last_batch_size_ = keys.size();
  for (std::size_t i = 0; i < keys.size(); ++i)
    scores[i] = fn_(keys[i], features.subspan(i * kF, kF));
}

}  // namespace lcache
