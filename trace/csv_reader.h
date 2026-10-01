#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

#include "trace/trace_reader.h"

namespace lcache {

// Simple CSV format for hand-written traces:
//
//   time,key,size[,op]
//
// - key: a decimal u64 is used as-is; anything else is hashed with FNV-1a (so "user:42" works).
// - op:  get | set | del (case-insensitive); defaults to get.
// - Blank lines and lines starting with '#' are ignored. No header row.
class CsvReader final : public TraceReader {
 public:
  explicit CsvReader(std::filesystem::path path);

  bool next(Request& out) override;
  void reset() override;

 private:
  std::filesystem::path path_;
  std::ifstream in_;
  std::uint64_t line_no_ = 0;  // for error messages
};

}  // namespace lcache
