#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "trace/request.h"

namespace lcache {

// Count-min sketch with saturating 4-bit counters and periodic aging, as used by TinyLFU
// (Einziger, Friedman & Manes 2017) and Caffeine.
//
// - depth 4; width rounded up to a power of two (minimum 16)
// - counters saturate at 15 (stored in uint8_t for simplicity; Caffeine packs them into 4 bits)
// - aging ("reset"): after sample_size() = 10 * width increments, every counter is halved, so
//   popularity from long ago fades
// - row indices come from double hashing a splitmix64 mix of the key: deterministic everywhere
//
// estimate() never under-counts a key's increments since the last aging (it may over-count).
class CountMinSketch {
 public:
  static constexpr std::size_t kDepth = 4;
  static constexpr std::uint8_t kMaxCount = 15;
  static constexpr std::size_t kMinWidth = 16;

  explicit CountMinSketch(std::size_t width);

  void increment(Key k);
  std::uint8_t estimate(Key k) const;

  std::size_t width() const { return width_; }
  std::uint64_t sample_size() const { return 10 * static_cast<std::uint64_t>(width_); }

 private:
  // Halves every counter and the addition count.
  void age();
  std::size_t index(std::uint64_t hash, std::size_t row) const;

  std::size_t width_;
  std::vector<std::uint8_t> table_;  // kDepth rows of width_ counters, row-major
  std::uint64_t additions_ = 0;
};

}  // namespace lcache
