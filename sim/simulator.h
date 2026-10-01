#pragma once

#include <cstdint>

#include "policy/eviction_policy.h"
#include "trace/trace_reader.h"

namespace lcache {

struct SimOptions {
  std::uint64_t capacity_bytes = 0;
  // Treat every object as size 1, so capacity_bytes is a capacity in objects
  // (libCacheSim's --ignore-obj-size). Byte metrics still use the real sizes.
  bool ignore_size = false;
  // Stop after this many requests; 0 means the whole trace.
  std::uint64_t max_requests = 0;
};

struct SimResult {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_hit = 0;

  // Both return 0 for an empty run rather than dividing by zero.
  double hit_rate() const;
  double byte_hit_rate() const;
};

// Replays `reader` from its first request against a cache that uses `policy`.
// `policy` must be freshly constructed (empty); use a new policy object for every run.
SimResult simulate(TraceReader& reader, EvictionPolicy& policy, const SimOptions& options);

}  // namespace lcache
