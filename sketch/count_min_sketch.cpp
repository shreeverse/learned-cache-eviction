#include "sketch/count_min_sketch.h"

#include <algorithm>
#include <bit>

namespace lcache {

namespace {

// splitmix64 finaliser: a cheap, well-mixed 64-bit hash, so keys that are sequential integers
// (common in traces) still spread over all columns.
std::uint64_t mix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

CountMinSketch::CountMinSketch(std::size_t width)
    : width_(std::bit_ceil(std::max(width, kMinWidth))), table_(kDepth * width_, 0) {}

std::size_t CountMinSketch::index(std::uint64_t hash, std::size_t row) const {
  // Double hashing: h1 + row * h2 (h2 odd, so the rows' columns differ even for small widths).
  const std::uint64_t h1 = hash & 0xffffffffULL;
  const std::uint64_t h2 = (hash >> 32) | 1U;
  return row * width_ + static_cast<std::size_t>((h1 + row * h2) & (width_ - 1));
}

void CountMinSketch::increment(Key k) {
  const std::uint64_t h = mix(k);
  for (std::size_t row = 0; row < kDepth; ++row) {
    std::uint8_t& c = table_[index(h, row)];
    if (c < kMaxCount) ++c;
  }
  if (++additions_ >= sample_size()) age();
}

std::uint8_t CountMinSketch::estimate(Key k) const {
  const std::uint64_t h = mix(k);
  std::uint8_t best = kMaxCount;
  for (std::size_t row = 0; row < kDepth; ++row) best = std::min(best, table_[index(h, row)]);
  return best;
}

void CountMinSketch::age() {
  for (auto& c : table_) c = static_cast<std::uint8_t>(c >> 1);
  additions_ /= 2;
}

}  // namespace lcache
