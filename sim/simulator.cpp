#include "sim/simulator.h"

#include "sim/cache.h"
#include "trace/next_access.h"
#include "trace/vector_reader.h"

namespace lcache {

namespace {

double ratio(std::uint64_t num, std::uint64_t den) {
  return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
}

SimResult replay(TraceReader& reader, EvictionPolicy& policy, const SimOptions& options) {
  reader.reset();  // so one reader can be replayed for several cache sizes
  Cache cache(policy, options.capacity_bytes);
  SimResult result;

  std::uint64_t replayed = 0;
  Request r;
  while ((options.max_requests == 0 || replayed < options.max_requests) && reader.next(r)) {
    const std::uint32_t real_size = r.size;
    if (options.ignore_size) r.size = 1;

    const bool hit = cache.access(r);
    if (replayed++ < options.warmup_requests) continue;
    ++result.requests;
    result.bytes_requested += real_size;
    if (hit) {
      ++result.hits;
      result.bytes_hit += real_size;
    }
  }
  return result;
}

}  // namespace

double SimResult::hit_rate() const { return ratio(hits, requests); }
double SimResult::byte_hit_rate() const { return ratio(bytes_hit, bytes_requested); }

SimResult simulate(TraceReader& reader, EvictionPolicy& policy, const SimOptions& options) {
  if (policy.needs_next_access()) {
    // Future-knowledge policies replay an annotated in-memory copy of the trace (decisions D7).
    VectorReader annotated(annotate_next_access(reader, options.max_requests));
    return replay(annotated, policy, options);
  }
  return replay(reader, policy, options);
}

}  // namespace lcache
