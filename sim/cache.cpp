#include "sim/cache.h"

#include <stdexcept>

namespace lcache {

Cache::Cache(EvictionPolicy& policy, std::uint64_t capacity_bytes)
    : policy_(policy), capacity_bytes_(capacity_bytes) {}

// TODO(shree): implement. This stub throws so the tests fail loudly until then.
bool Cache::access(const Request& /*r*/) {
  throw std::logic_error("Cache::access not implemented");
}

}  // namespace lcache
