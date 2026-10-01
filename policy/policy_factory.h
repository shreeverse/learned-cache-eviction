#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "policy/eviction_policy.h"

namespace lcache {

// Everything a policy may need at construction. Each policy reads only the fields it uses.
struct PolicyConfig {
  // Cache capacity; ARC and W-TinyLFU size their internal segments from it.
  std::uint64_t capacity_bytes = 0;
  // "learned": the LightGBM model file written by tools/train.py, and how to evaluate it:
  // "compiled" (our TreeEnsemblePredictor, default; regression models only, bit-identical to
  // LightGBM and much faster) or "lightgbm" (the LightGBM library; any model).
  std::string model_path{};
  std::string predictor = "compiled";
  // Sampled policies (oracle-sampled, random-sampled, learned): candidates per eviction, and the
  // seed for sampling and random scores.
  std::uint32_t sample_size = 64;
  std::uint64_t seed = 1;
  // Sampled policies: evicted keys whose feature history is kept (decisions D17, D20): a fixed
  // count, or history_ratio * cached objects if history_ratio > 0. Feature normalization is not
  // here: "learned" takes it from the model.
  std::size_t history = 0;
  double history_ratio = 0;
  // "guarded" (FallbackPolicy around "learned", decisions D21): rolling window and margin for the
  // hit-rate comparison, cooldown before retrying, and the per-eviction time budget.
  std::uint64_t guard_window = 10'000;
  double guard_margin = 0.0;
  std::uint64_t guard_grace = 30'000;
  bool guard_early_check = true;
  std::uint64_t guard_cooldown = 50'000;
  std::uint64_t budget_ns = 0;  // 0 = no budget
  std::uint32_t max_breaches = 1;
  std::uint64_t breach_window = 1'000;
};

// Creates a fresh policy by name. Throws std::invalid_argument for an unknown name or an
// invalid config.
std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, const PolicyConfig& config);

// Shorthand when only the capacity matters.
inline std::unique_ptr<EvictionPolicy> make_policy(std::string_view name,
                                                   std::uint64_t capacity_bytes) {
  return make_policy(name, PolicyConfig{.capacity_bytes = capacity_bytes});
}

// Names make_policy accepts with a default PolicyConfig. "learned" and "guarded" are not listed:
// they also need config.model_path.
std::vector<std::string> policy_names();

// True if built with the LightGBM library (config.predictor = "lightgbm" is available).
bool has_lightgbm();

}  // namespace lcache
