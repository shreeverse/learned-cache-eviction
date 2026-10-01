#pragma once

#include <cstdint>
#include <string_view>

namespace lcache {

// Parses a byte count: a plain integer ("1048576") or an integer with a binary suffix
// KiB, MiB, GiB or TiB ("64MiB"). Decimal suffixes (KB, MB, ...) are rejected on purpose, since
// tools disagree on whether they mean 1000 or 1024. Throws std::invalid_argument on bad input.
std::uint64_t parse_size(std::string_view text);

}  // namespace lcache
