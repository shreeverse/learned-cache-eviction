#include "policy/belady.h"

#include <stdexcept>

namespace lcache {

// Stubs: implementation follows in the next commit (tests first).
void BeladyPolicy::on_hit(const Request& /*r*/) {
  throw std::logic_error("BeladyPolicy: not implemented");
}
void BeladyPolicy::on_insert(const Request& /*r*/) {
  throw std::logic_error("BeladyPolicy: not implemented");
}
Key BeladyPolicy::evict() { throw std::logic_error("BeladyPolicy: not implemented"); }
void BeladyPolicy::on_remove(Key /*k*/) { throw std::logic_error("BeladyPolicy: not implemented"); }

}  // namespace lcache
