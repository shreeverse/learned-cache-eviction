#include "policy/wtinylfu.h"

#include <stdexcept>

namespace lcache {

// Stubs: implementation follows in the next commit (tests first).
WTinyLfuPolicy::WTinyLfuPolicy(std::uint64_t capacity_bytes, const Options& options)
    : window_budget_(0), protected_budget_(0), sketch_(options.initial_sketch_width) {
  (void)capacity_bytes;
}
void WTinyLfuPolicy::on_hit(const Request& /*r*/) {
  throw std::logic_error("WTinyLfuPolicy: not implemented");
}
void WTinyLfuPolicy::on_insert(const Request& /*r*/) {
  throw std::logic_error("WTinyLfuPolicy: not implemented");
}
Key WTinyLfuPolicy::evict() { throw std::logic_error("WTinyLfuPolicy: not implemented"); }
void WTinyLfuPolicy::on_remove(Key /*k*/) {
  throw std::logic_error("WTinyLfuPolicy: not implemented");
}
WTinyLfuPolicy::State WTinyLfuPolicy::state() const { return {}; }
void WTinyLfuPolicy::record_access(Key /*k*/) {}
void WTinyLfuPolicy::move_to(Entry& /*e*/, Segment /*to*/) {}
void WTinyLfuPolicy::drop(Key /*k*/) {}
void WTinyLfuPolicy::drain_window() {}
void WTinyLfuPolicy::demote_protected_overflow() {}
Key WTinyLfuPolicy::main_victim() const { return 0; }
bool WTinyLfuPolicy::has_main_victim() const { return false; }

}  // namespace lcache
