#pragma once

#include <cstdint>
#include <vector>

#include "policy/eviction_policy.h"
#include "trace/trace_reader.h"

namespace lcache {

struct SimOptions {
  std::uint64_t capacity_bytes = 0;
  // Treat every object as size 1, so capacity_bytes is a capacity in objects
  // (libCacheSim's --ignore-obj-size). Byte metrics still use the real sizes.
  bool ignore_size = false;
  // Stop after this many requests (warmup included); 0 means the whole trace.
  std::uint64_t max_requests = 0;
  // Replay the first N requests to warm the cache, but leave them out of every count in
  // SimResult, so policies are scored on a held-out suffix of the trace (decisions D10).
  std::uint64_t warmup_requests = 0;
  // If > 0, also record counts per interval of this many counted requests in SimResult::series,
  // e.g. for a hit-rate-over-time plot. The API sizes it to bound the points sent (decisions D26).
  std::uint64_t series_interval = 0;
};

// Counts for one interval of counted requests.
struct SeriesBucket {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_hit = 0;
};

// Counts cover only requests after the warmup.
struct SimResult {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_hit = 0;
  // Empty unless SimOptions::series_interval > 0. Every bucket holds series_interval requests
  // except possibly the last; the buckets sum to the totals above.
  std::vector<SeriesBucket> series{};

  // Both return 0 for an empty run rather than dividing by zero.
  double hit_rate() const;
  double byte_hit_rate() const;
};

// Replays `reader` from its first request against a cache that uses `policy`.
// `policy` must be freshly constructed (empty); use a new policy object for every run.
SimResult simulate(TraceReader& reader, EvictionPolicy& policy, const SimOptions& options);

}  // namespace lcache
