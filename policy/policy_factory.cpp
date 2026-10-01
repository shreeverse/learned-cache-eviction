#include "policy/policy_factory.h"

#include <stdexcept>

#include "ml/predictor.h"
#include "policy/arc.h"
#include "policy/belady.h"
#include "policy/learned.h"
#include "policy/lfu.h"
#include "policy/lru.h"
#include "policy/wtinylfu.h"

namespace lcache {

std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, const PolicyConfig& config) {
  const LearnedOptions sampled{.sample_size = config.sample_size, .seed = config.seed};
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
  throw std::invalid_argument("unknown policy '" + std::string(name) + "'");
}

std::vector<std::string> policy_names() {
  return {"lru", "lfu", "arc", "wtinylfu", "belady", "oracle-sampled", "random-sampled"};
}

}  // namespace lcache
