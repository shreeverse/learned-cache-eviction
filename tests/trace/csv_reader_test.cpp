#include "trace/csv_reader.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "test_util.h"
#include "trace/hash.h"

namespace lcache {
namespace {

std::vector<Request> read_all(TraceReader& reader) {
  std::vector<Request> out;
  Request r;
  while (reader.next(r)) out.push_back(r);
  return out;
}

TEST(CsvReader, ParsesNumericKeysAndDefaultsOpToGet) {
  test::TempFile f(".csv");
  f.write("1,42,100\n2,43,200\n");
  CsvReader reader(f.path());
  EXPECT_EQ(read_all(reader), (std::vector<Request>{{1, 42, 100, Op::Get}, {2, 43, 200, Op::Get}}));
}

TEST(CsvReader, HashesNonNumericKeys) {
  test::TempFile f(".csv");
  f.write("1,user:42,10\n");
  CsvReader reader(f.path());
  const auto reqs = read_all(reader);
  ASSERT_EQ(reqs.size(), 1U);
  EXPECT_EQ(reqs[0].key, fnv1a64("user:42"));
}

TEST(CsvReader, ParsesOpsCaseInsensitively) {
  test::TempFile f(".csv");
  f.write("1,1,1,get\n2,1,1,SET\n3,1,1,Del\n");
  CsvReader reader(f.path());
  const auto reqs = read_all(reader);
  ASSERT_EQ(reqs.size(), 3U);
  EXPECT_EQ(reqs[0].op, Op::Get);
  EXPECT_EQ(reqs[1].op, Op::Set);
  EXPECT_EQ(reqs[2].op, Op::Del);
}

TEST(CsvReader, SkipsCommentsBlankLinesAndWhitespace) {
  test::TempFile f(".csv");
  f.write("# time,key,size\n\n  1 , 5 , 7 \r\n   \n2,6,8");  // CRLF and no trailing newline
  CsvReader reader(f.path());
  EXPECT_EQ(read_all(reader), (std::vector<Request>{{1, 5, 7, Op::Get}, {2, 6, 8, Op::Get}}));
}

TEST(CsvReader, ResetRewinds) {
  test::TempFile f(".csv");
  f.write("1,1,1\n2,2,2\n");
  CsvReader reader(f.path());
  const auto first = read_all(reader);
  reader.reset();
  EXPECT_EQ(read_all(reader), first);
}

TEST(CsvReader, MalformedLinesThrow) {
  for (const char* bad : {"1,2\n", "1,2,3,get,extra\n", "x,2,3\n", "1,2,-5\n", "1,2,99999999999\n",
                          "1,,3\n", "1,2,3,put\n"}) {
    SCOPED_TRACE(bad);
    test::TempFile f(".csv");
    f.write(bad);
    CsvReader reader(f.path());
    Request r;
    EXPECT_THROW(reader.next(r), std::runtime_error);
  }
}

TEST(CsvReader, MissingFileThrows) {
  EXPECT_THROW(CsvReader("/nonexistent/trace.csv"), std::runtime_error);
}

TEST(Fnv1a64, MatchesReferenceVectors) {
  // Published FNV-1a 64-bit test vectors.
  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
}

}  // namespace
}  // namespace lcache
