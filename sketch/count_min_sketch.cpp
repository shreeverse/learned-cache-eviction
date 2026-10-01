#include "sketch/count_min_sketch.h"

namespace lcache {

// Stubs: implementation follows in the next commit (tests first).
CountMinSketch::CountMinSketch(std::size_t width) : width_(width) {}
void CountMinSketch::increment(Key /*k*/) {}
std::uint8_t CountMinSketch::estimate(Key /*k*/) const { return 0; }
void CountMinSketch::age() {}
std::size_t CountMinSketch::index(std::uint64_t /*hash*/, std::size_t /*row*/) const { return 0; }

}  // namespace lcache
