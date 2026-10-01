#include "sim/simulator.h"

#include "sim/cache.h"

namespace lcache {

namespace {

double ratio(std::uint64_t num, std::uint64_t den) {
  return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
}

}  // namespace

double SimResult::hit_rate() const { return ratio(hits, requests); }
double SimResult::byte_hit_rate() const { return ratio(bytes_hit, bytes_requested); }

SimResult simulate(TraceReader& reader, EvictionPolicy& policy, const SimOptions& options) {
  reader.reset();  // so one reader can be replayed for several cache sizes
  Cache cache(policy, options.capacity_bytes);
  SimResult result;

  Request r;
  while ((options.max_requests == 0 || result.requests < options.max_requests) && reader.next(r)) {
    const std::uint32_t real_size = r.size;
    if (options.ignore_size) r.size = 1;

    const bool hit = cache.access(r);
    ++result.requests;
    result.bytes_requested += real_size;
    if (hit) {
      ++result.hits;
      result.bytes_hit += real_size;
    }
  }
  return result;
}

}  // namespace lcache
