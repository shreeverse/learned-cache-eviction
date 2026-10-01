#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "trace/trace_reader.h"

namespace lcache {

// TraceReader over requests held in memory (tests, and traces annotated by annotate_next_access).
class VectorReader final : public TraceReader {
 public:
  explicit VectorReader(std::vector<Request> reqs) : reqs_(std::move(reqs)) {}

  bool next(Request& out) override {
    if (pos_ == reqs_.size()) return false;
    out = reqs_[pos_++];
    return true;
  }
  void reset() override { pos_ = 0; }

  const std::vector<Request>& requests() const { return reqs_; }

 private:
  std::vector<Request> reqs_;
  std::size_t pos_ = 0;
};

}  // namespace lcache
