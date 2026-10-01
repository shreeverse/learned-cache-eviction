#pragma once

#include <cstdint>
#include <string_view>

namespace lcache {

// FNV-1a, 64-bit. Used instead of std::hash because std::hash differs between standard libraries,
// and simulation results must be reproducible everywhere.
constexpr std::uint64_t fnv1a64(std::string_view s) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace lcache
