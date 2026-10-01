#include "trace/oracle_general_reader.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_util.h"

namespace lcache {
namespace {

struct Record {
  std::uint32_t time;
  std::uint64_t id;
  std::uint32_t size;
  std::int64_t next_vtime;
};

// Serializes records exactly as libCacheSim lays them out (packed, little-endian).
std::string encode(const std::vector<Record>& recs) {
  std::string bytes;
  for (const auto& r : recs) {
    std::array<char, OracleGeneralReader::kRecordSize> buf{};
    std::memcpy(buf.data(), &r.time, 4);
    std::memcpy(buf.data() + 4, &r.id, 8);
    std::memcpy(buf.data() + 12, &r.size, 4);
    std::memcpy(buf.data() + 16, &r.next_vtime, 8);
    bytes.append(buf.data(), buf.size());
  }
  return bytes;
}

std::vector<Request> read_all(TraceReader& reader) {
  std::vector<Request> out;
  Request r;
  while (reader.next(r)) out.push_back(r);
  return out;
}

TEST(OracleGeneralReader, DecodesAllFields) {
  test::TempFile f(".bin");
  f.write(encode({{10, 0xDEADBEEFCAFEULL, 512, 3}, {11, 7, 4096, -1}}));

  OracleGeneralReader reader(f.path());
  const auto reqs = read_all(reader);

  ASSERT_EQ(reqs.size(), 2U);
  EXPECT_EQ(reqs[0], (Request{10, 0xDEADBEEFCAFEULL, 512, Op::Get}));
  EXPECT_EQ(reqs[1], (Request{11, 7, 4096, Op::Get}));
}

TEST(OracleGeneralReader, EmptyFileHasNoRequests) {
  test::TempFile f(".bin");
  f.write("");
  OracleGeneralReader reader(f.path());
  Request r;
  EXPECT_FALSE(reader.next(r));
}

TEST(OracleGeneralReader, SkipsZeroSizeByDefaultLikeLibCacheSim) {
  test::TempFile f(".bin");
  f.write(encode({{1, 1, 100, -1}, {2, 2, 0, -1}, {3, 3, 300, -1}}));

  OracleGeneralReader reader(f.path());
  const auto reqs = read_all(reader);

  ASSERT_EQ(reqs.size(), 2U);
  EXPECT_EQ(reqs[0].key, 1U);
  EXPECT_EQ(reqs[1].key, 3U);
}

TEST(OracleGeneralReader, KeepsZeroSizeWhenAsked) {
  test::TempFile f(".bin");
  f.write(encode({{1, 1, 100, -1}, {2, 2, 0, -1}}));
  OracleGeneralReader reader(f.path(), {.skip_zero_size = false});
  EXPECT_EQ(read_all(reader).size(), 2U);
}

TEST(OracleGeneralReader, ReadsAcrossBufferRefills) {
  // More records than fit in one internal buffer chunk (65536), so refill() runs several times.
  constexpr std::uint32_t kN = 200'000;
  std::vector<Record> recs;
  recs.reserve(kN);
  for (std::uint32_t i = 0; i < kN; ++i) recs.push_back({i, i * 3ULL, i + 1, -1});
  test::TempFile f(".bin");
  f.write(encode(recs));

  OracleGeneralReader reader(f.path());
  const auto reqs = read_all(reader);

  ASSERT_EQ(reqs.size(), kN);
  for (std::uint32_t i = 0; i < kN; ++i) {
    ASSERT_EQ(reqs[i], (Request{i, i * 3ULL, i + 1, Op::Get})) << "at index " << i;
  }
}

TEST(OracleGeneralReader, ResetRewinds) {
  test::TempFile f(".bin");
  f.write(encode({{1, 1, 1, -1}, {2, 2, 2, -1}}));
  OracleGeneralReader reader(f.path());

  const auto first = read_all(reader);
  reader.reset();
  EXPECT_EQ(read_all(reader), first);
}

TEST(OracleGeneralReader, TruncatedFileThrows) {
  test::TempFile f(".bin");
  f.write(encode({{1, 1, 1, -1}}) + "xyz");
  OracleGeneralReader reader(f.path());
  Request r;
  EXPECT_THROW(reader.next(r), std::runtime_error);
}

TEST(OracleGeneralReader, MissingFileThrows) {
  EXPECT_THROW(OracleGeneralReader("/nonexistent/trace.bin"), std::runtime_error);
}

}  // namespace
}  // namespace lcache
