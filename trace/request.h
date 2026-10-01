#pragma once

#include <cstdint>
#include <limits>

namespace lcache {

using Key = std::uint64_t;

// Sentinel for Request::next_vtime: the key is not requested again (in this run).
inline constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

enum class Op : std::uint8_t { Get, Set, Del };

// One cache request, independent of the trace format it came from.
struct Request {
  std::uint64_t time = 0;  // timestamp as given by the trace (units are trace-specific)
  Key key = 0;
  std::uint32_t size = 0;  // object size in bytes
  Op op = Op::Get;
  // Index (0-based, in the replayed stream) of the next request for the same key, or kNever.
  // Only filled in by annotate_next_access(); readers leave it as kNever.
  std::uint64_t next_vtime = kNever;

  friend bool operator==(const Request&, const Request&) = default;
};

}  // namespace lcache
