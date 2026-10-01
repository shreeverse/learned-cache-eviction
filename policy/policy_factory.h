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

// Names accepted by make_policy, for help text and validation.
std::vector<std::string> policy_names();

}  // namespace lcache
