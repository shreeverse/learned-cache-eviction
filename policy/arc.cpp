#include "policy/arc.h"

#include <stdexcept>

namespace lcache {

// Stubs: implementation follows in the next commit (tests first).
void ArcPolicy::on_hit(const Request& /*r*/) {
  throw std::logic_error("ArcPolicy: not implemented");
}
void ArcPolicy::on_insert(const Request& /*r*/) {
  throw std::logic_error("ArcPolicy: not implemented");
}
Key ArcPolicy::evict() { throw std::logic_error("ArcPolicy: not implemented"); }
void ArcPolicy::on_remove(Key /*k*/) { throw std::logic_error("ArcPolicy: not implemented"); }
ArcPolicy::State ArcPolicy::state() const { return {}; }
void ArcPolicy::move_to(Index::iterator /*it*/, ListId /*to*/) {}
void ArcPolicy::drop(Index::iterator /*it*/) {}
void ArcPolicy::trim_ghosts() {}

}  // namespace lcache
