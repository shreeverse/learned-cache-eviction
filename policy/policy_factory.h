#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "policy/eviction_policy.h"

namespace lcache {

// Creates a fresh policy by name for a cache of `capacity_bytes` (some policies size their
// internal segments from it). Throws std::invalid_argument for an unknown name.
std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, std::uint64_t capacity_bytes);

// Names accepted by make_policy, for help text and validation.
std::vector<std::string> policy_names();

}  // namespace lcache
