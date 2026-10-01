#include "policy/policy_factory.h"

#include <stdexcept>
#include <utility>

#include "ml/predictor.h"
#include "ml/tree_ensemble.h"
#ifdef LC_HAVE_LIGHTGBM
#include "ml/lightgbm_predictor.h"
#endif
#include "policy/arc.h"
#include "policy/belady.h"
#include "policy/fallback.h"
#include "policy/learned.h"
#include "policy/lfu.h"
#include "policy/lru.h"
#include "policy/wtinylfu.h"

namespace lcache {

namespace {

std::unique_ptr<EvictionPolicy> make_learned(const PolicyConfig& config) {
  const LearnedOptions sampled{
      .sample_size = config.sample_size,
      .seed = config.seed,
      .features = {.history = config.history, .history_ratio = config.history_ratio}};
  if (config.model_path.empty()) {
    throw std::invalid_argument("policy 'learned' needs a model file (--model)");
  }
  std::unique_ptr<Predictor> predictor;
  if (config.predictor == "compiled") {
    predictor = std::make_unique<TreeEnsemblePredictor>(config.model_path);
  } else if (config.predictor == "lightgbm") {
#ifdef LC_HAVE_LIGHTGBM
    predictor = std::make_unique<LightGbmPredictor>(config.model_path);
#else
    throw std::invalid_argument("predictor 'lightgbm' needs a build with LC_WITH_LIGHTGBM=ON");
#endif
  } else {
    throw std::invalid_argument("unknown predictor '" + config.predictor + "'");
  }
  LearnedOptions options = sampled;
  options.features.normalize = predictor->normalized_features().value_or(false);
  return std::make_unique<LearnedPolicy>(std::move(predictor), "learned", options);
}

}  // namespace

std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, const PolicyConfig& config) {
  const LearnedOptions sampled{
      .sample_size = config.sample_size,
      .seed = config.seed,
      .features = {.history = config.history, .history_ratio = config.history_ratio}};
  if (name == "lru") return std::make_unique<LruPolicy>();
  if (name == "lfu") return std::make_unique<LfuPolicy>();
  if (name == "arc") return std::make_unique<ArcPolicy>(config.capacity_bytes);
  if (name == "wtinylfu") return std::make_unique<WTinyLfuPolicy>(config.capacity_bytes);
  if (name == "belady") return std::make_unique<BeladyPolicy>();
  if (name == "oracle-sampled") {
    return std::make_unique<LearnedPolicy>(std::make_unique<OraclePredictor>(), std::string(name),
                                           sampled);
  }
  if (name == "random-sampled") {
    // Offset the seed so the scores aren't correlated with the candidate sampling.
    return std::make_unique<LearnedPolicy>(std::make_unique<RandomPredictor>(config.seed + 1),
                                           std::string(name), sampled);
  }
  if (name == "learned") return make_learned(config);
  if (name == "guarded") {
    return std::make_unique<FallbackPolicy>(make_learned(config), config.capacity_bytes,
                                            FallbackOptions{.window = config.guard_window,
                                                            .margin = config.guard_margin,
                                                            .grace = config.guard_grace,
                                                            .early_check = config.guard_early_check,
                                                            .cooldown = config.guard_cooldown,
                                                            .budget_ns = config.budget_ns,
                                                            .max_breaches = config.max_breaches,
                                                            .breach_window = config.breach_window,
                                                            .clock_ns = {}});
  }
  throw std::invalid_argument("unknown policy '" + std::string(name) + "'");
}

std::vector<std::string> policy_names() {
  return {"lru", "lfu", "arc", "wtinylfu", "belady", "oracle-sampled", "random-sampled"};
}

bool has_lightgbm() {
#ifdef LC_HAVE_LIGHTGBM
  return true;
#else
  return false;
#endif
}

}  // namespace lcache
