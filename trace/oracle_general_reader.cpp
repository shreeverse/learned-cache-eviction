#include "trace/oracle_general_reader.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace lcache {

namespace {

// The format is little-endian and we copy fields straight from the buffer.
static_assert(std::endian::native == std::endian::little,
              "oracleGeneral reader assumes little-endian");

constexpr std::size_t kRecordsPerChunk = 1U << 16;  // 1.5 MiB buffer

// memcpy instead of casting the buffer pointer: fields are unaligned, so a cast would be UB.
template <typename T>
T load(const char* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

}  // namespace

OracleGeneralReader::OracleGeneralReader(std::filesystem::path path, OracleGeneralOptions options)
    : path_(std::move(path)), options_(options), buf_(kRecordSize * kRecordsPerChunk) {
  in_.open(path_, std::ios::binary);
  if (!in_) {
    throw std::runtime_error("cannot open trace file: " + path_.string());
  }
}

bool OracleGeneralReader::refill() {
  in_.read(buf_.data(), static_cast<std::streamsize>(buf_.size()));
  len_ = static_cast<std::size_t>(in_.gcount());
  pos_ = 0;
  // A short read is only acceptable at EOF, and must still end on a record boundary.
  if (len_ % kRecordSize != 0) {
    throw std::runtime_error("truncated oracleGeneral trace (size not a multiple of 24 bytes): " +
                             path_.string());
  }
  return len_ > 0;
}

bool OracleGeneralReader::next(Request& out) {
  while (true) {
    if (pos_ == len_ && !refill()) {
      return false;
    }
    const char* rec = buf_.data() + pos_;
    pos_ += kRecordSize;

    const auto size = load<std::uint32_t>(rec + 12);
    if (size == 0 && options_.skip_zero_size) {
      continue;
    }
    out.time = load<std::uint32_t>(rec);
    out.key = load<std::uint64_t>(rec + 4);
    out.size = size;
    out.op = Op::Get;
    return true;
  }
}

void OracleGeneralReader::reset() {
  in_.clear();
  in_.seekg(0);
  pos_ = 0;
  len_ = 0;
}

}  // namespace lcache
