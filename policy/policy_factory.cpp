#include "policy/policy_factory.h"

#include <stdexcept>

#include "policy/lfu.h"
#include "policy/lru.h"

namespace lcache {

std::unique_ptr<EvictionPolicy> make_policy(std::string_view name,
                                            std::uint64_t /*capacity_bytes*/) {
  if (name == "lru") return std::make_unique<LruPolicy>();
  if (name == "lfu") return std::make_unique<LfuPolicy>();
  throw std::invalid_argument("unknown policy '" + std::string(name) + "'");
}

std::vector<std::string> policy_names() { return {"lru", "lfu"}; }

}  // namespace lcache
