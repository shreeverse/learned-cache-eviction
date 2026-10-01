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
  // "learned": the LightGBM model file written by tools/train.py.
  std::string model_path{};
  // Sampled policies (oracle-sampled, random-sampled, learned): candidates per eviction, and the
  // seed for sampling and random scores.
  std::uint32_t sample_size = 64;
  std::uint64_t seed = 1;
};

// Creates a fresh policy by name. Throws std::invalid_argument for an unknown name or an
// invalid config.
std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, const PolicyConfig& config);

// Shorthand when only the capacity matters.
inline std::unique_ptr<EvictionPolicy> make_policy(std::string_view name,
                                                   std::uint64_t capacity_bytes) {
  return make_policy(name, PolicyConfig{.capacity_bytes = capacity_bytes});
}

// Names make_policy accepts with a default PolicyConfig. "learned" is not listed: it also needs
// config.model_path, and exists only when built with LightGBM (has_learned_policy()).
std::vector<std::string> policy_names();

bool has_learned_policy();

}  // namespace lcache
