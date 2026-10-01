#include "apps/cachesim/size_parse.h"

#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace lcache {

std::uint64_t parse_size(std::string_view text) {
  const auto fail = [&]() {
    throw std::invalid_argument("bad size '" + std::string(text) +
                                "' (expected e.g. 1048576, 512KiB, 64MiB, 1GiB)");
  };

  std::uint64_t value = 0;
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr == text.data()) fail();

  const std::string_view suffix(ptr, static_cast<std::size_t>(end - ptr));
  if (suffix.empty()) return value;

  constexpr std::array<std::pair<std::string_view, int>, 4> kUnits{
      {{"KiB", 10}, {"MiB", 20}, {"GiB", 30}, {"TiB", 40}}};
  for (const auto& [unit, shift] : kUnits) {
    if (suffix == unit) {
      if (value > (std::numeric_limits<std::uint64_t>::max() >> shift)) fail();  // overflow
      return value << shift;
    }
  }
  fail();
  return 0;  // unreachable
}

}  // namespace lcache
