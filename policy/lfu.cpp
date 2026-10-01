#include "policy/lfu.h"

#include <stdexcept>

namespace lcache {

// Stubs: implementation follows in the next commit (tests first).
void LfuPolicy::on_hit(const Request& /*r*/) {
  throw std::logic_error("LfuPolicy: not implemented");
}
void LfuPolicy::on_insert(const Request& /*r*/) {
  throw std::logic_error("LfuPolicy: not implemented");
}
Key LfuPolicy::evict() { throw std::logic_error("LfuPolicy: not implemented"); }
void LfuPolicy::on_remove(Key /*k*/) { throw std::logic_error("LfuPolicy: not implemented"); }
void LfuPolicy::erase(std::unordered_map<Key, Entry>::iterator /*it*/) {}

}  // namespace lcache
