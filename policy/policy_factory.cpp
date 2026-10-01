#include "policy/policy_factory.h"

#include <stdexcept>

#include "policy/lru.h"

namespace lcache {

std::unique_ptr<EvictionPolicy> make_policy(std::string_view name) {
  if (name == "lru") return std::make_unique<LruPolicy>();
  throw std::invalid_argument("unknown policy '" + std::string(name) + "'");
}

std::vector<std::string> policy_names() { return {"lru"}; }

}  // namespace lcache
