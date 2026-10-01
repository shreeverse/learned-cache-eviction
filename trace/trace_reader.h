#pragma once

#include "trace/request.h"

namespace lcache {

// Streaming, pull-based trace reader. Traces can be many GB, so readers never load a whole file.
class TraceReader {
 public:
  TraceReader() = default;
  TraceReader(const TraceReader&) = delete;
  TraceReader& operator=(const TraceReader&) = delete;
  TraceReader(TraceReader&&) = delete;
  TraceReader& operator=(TraceReader&&) = delete;
  virtual ~TraceReader() = default;

  // Reads the next request into `out`. Returns false at end of trace (then `out` is unspecified).
  // Throws std::runtime_error on malformed input.
  virtual bool next(Request& out) = 0;

  // Rewinds to the first request.
  virtual void reset() = 0;
};

}  // namespace lcache
