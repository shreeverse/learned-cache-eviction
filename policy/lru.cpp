#include "policy/lru.h"

#include <stdexcept>

namespace lcache {

// TODO(shree): implement. These stubs throw so the tests fail loudly until then.

void LruPolicy::on_hit(const Request& /*r*/) {
  throw std::logic_error("LruPolicy::on_hit not implemented");
}

void LruPolicy::on_insert(const Request& /*r*/) {
  throw std::logic_error("LruPolicy::on_insert not implemented");
}

Key LruPolicy::evict() { throw std::logic_error("LruPolicy::evict not implemented"); }

void LruPolicy::on_remove(Key /*k*/) {
  throw std::logic_error("LruPolicy::on_remove not implemented");
}

}  // namespace lcache
