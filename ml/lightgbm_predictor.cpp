#include "ml/lightgbm_predictor.h"

#include <LightGBM/c_api.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ml/feature_tracker.h"

namespace lcache {

namespace {

constexpr std::size_t kF = FeatureTracker::kNumFeatures;

// Single-threaded prediction: no thread pool per call (decisions D14).
constexpr const char* kPredictParams = "num_threads=1";

// LightGBM's C API returns 0 on success, otherwise the message is in LGBM_GetLastError().
void check(int status, std::string_view what) {
  if (status != 0) {
    throw std::runtime_error(std::string(what) + ": " + LGBM_GetLastError());
  }
}

std::string join(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += (out.empty() ? "" : " ") + n;
  return out;
}

std::vector<std::string> model_feature_names(BoosterHandle booster) {
  int n = 0;
  check(LGBM_BoosterGetNumFeature(booster, &n), "LGBM_BoosterGetNumFeature");
  // Ask with a fixed buffer size first; LightGBM reports the size it really needs.
  std::size_t buf_len = 64;
  for (;;) {
    std::vector<std::vector<char>> bufs(static_cast<std::size_t>(n), std::vector<char>(buf_len));
    std::vector<char*> ptrs;
    ptrs.reserve(bufs.size());
    for (auto& b : bufs) ptrs.push_back(b.data());
    int got = 0;
    std::size_t needed = 0;
    check(LGBM_BoosterGetFeatureNames(booster, n, &got, buf_len, &needed, ptrs.data()),
          "LGBM_BoosterGetFeatureNames");
    if (needed <= buf_len) {
      std::vector<std::string> names;
      names.reserve(static_cast<std::size_t>(got));
      for (int i = 0; i < got; ++i) names.emplace_back(bufs[static_cast<std::size_t>(i)].data());
      return names;
    }
    buf_len = needed;
  }
}

}  // namespace

LightGbmPredictor::LightGbmPredictor(const std::string& model_path) {
  BoosterHandle booster = nullptr;
  check(LGBM_BoosterCreateFromModelfile(model_path.c_str(), &num_iterations_, &booster),
        "cannot load LightGBM model '" + model_path + "'");
  booster_.reset(booster);

  const auto names = model_feature_names(booster);
  std::vector<std::string> expected;
  for (const std::string_view n : FeatureTracker::feature_names()) expected.emplace_back(n);
  if (names != expected) {
    throw std::runtime_error("model '" + model_path + "' has features [" + join(names) +
                             "], but FeatureTracker computes [" + join(expected) + "]");
  }

  FastConfigHandle config = nullptr;
  check(LGBM_BoosterPredictForMatSingleRowFastInit(
            booster, C_API_PREDICT_NORMAL, /*start_iteration=*/0, /*num_iteration=*/-1,
            C_API_DTYPE_FLOAT32, static_cast<std::int32_t>(kF), kPredictParams, &config),
        "LGBM_BoosterPredictForMatSingleRowFastInit");
  fast_config_.reset(config);
}

void LightGbmPredictor::BoosterDeleter::operator()(void* booster) const {
  LGBM_BoosterFree(booster);
}

void LightGbmPredictor::FastConfigDeleter::operator()(void* config) const {
  LGBM_FastConfigFree(config);
}

void LightGbmPredictor::predict(std::span<const Key> keys, std::span<const float> features,
                                std::span<double> scores) {
  assert(features.size() == keys.size() * kF && scores.size() == keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    std::int64_t written = 0;
    check(LGBM_BoosterPredictForMatSingleRowFast(fast_config_.get(), features.data() + i * kF,
                                                 &written, &scores[i]),
          "LGBM_BoosterPredictForMatSingleRowFast");
    assert(written == 1);
  }
}

}  // namespace lcache
