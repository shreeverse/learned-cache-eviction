#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include "trace/trace_reader.h"

namespace lcache {

struct OracleGeneralOptions {
  // libCacheSim skips size-0 requests by default; we match it so request counts agree.
  bool skip_zero_size = true;
};

// Reads libCacheSim's oracleGeneral binary format: packed little-endian 24-byte records
//   { u32 time; u64 obj_id; u32 obj_size; i64 next_access_vtime; }
// next_access_vtime is not exposed (see docs/decisions.md, D4).
class OracleGeneralReader final : public TraceReader {
 public:
  static constexpr std::size_t kRecordSize = 24;

  explicit OracleGeneralReader(std::filesystem::path path, OracleGeneralOptions options = {});

  bool next(Request& out) override;
  void reset() override;

 private:
  // Refills buf_ from the file. Returns false if there is no more data.
  bool refill();

  std::filesystem::path path_;
  OracleGeneralOptions options_;
  std::ifstream in_;
  std::vector<char> buf_;
  std::size_t pos_ = 0;  // read offset into buf_
  std::size_t len_ = 0;  // valid bytes in buf_
};

}  // namespace lcache
