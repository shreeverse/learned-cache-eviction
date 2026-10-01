#pragma once

#include <cstdint>
#include <vector>

#include "trace/trace_reader.h"

namespace lcache {

// Reads `reader` from the start (at most `max_requests`, 0 = all) into memory and fills each
// request's next_vtime: the index of the next request for the same key, or kNever.
// O(N) time and memory. Used by policies with future knowledge (Belady) and for ML labels.
std::vector<Request> annotate_next_access(TraceReader& reader, std::uint64_t max_requests = 0);

}  // namespace lcache
