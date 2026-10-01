#pragma once

#include <cstdint>

namespace lcache {

using Key = std::uint64_t;

enum class Op : std::uint8_t { Get, Set, Del };

// One cache request, independent of the trace format it came from.
struct Request {
  std::uint64_t time = 0;  // timestamp as given by the trace (units are trace-specific)
  Key key = 0;
  std::uint32_t size = 0;  // object size in bytes
  Op op = Op::Get;

  friend bool operator==(const Request&, const Request&) = default;
};

}  // namespace lcache
