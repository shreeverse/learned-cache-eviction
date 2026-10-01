#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "policy/eviction_policy.h"

namespace lcache {

// Creates a fresh policy by name. Throws std::invalid_argument for an unknown name.
std::unique_ptr<EvictionPolicy> make_policy(std::string_view name);

// Names accepted by make_policy, for help text and validation.
std::vector<std::string> policy_names();

}  // namespace lcache
