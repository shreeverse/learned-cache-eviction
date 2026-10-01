#include "trace/next_access.h"

#include <gtest/gtest.h>

#include <vector>

#include "trace/vector_reader.h"

namespace lcache {
namespace {

Request req(Key k) { return Request{.key = k, .size = 1}; }

std::vector<std::uint64_t> next_of(const std::vector<Request>& reqs) {
  std::vector<std::uint64_t> out;
  for (const auto& r : reqs) out.push_back(r.next_vtime);
  return out;
}

TEST(AnnotateNextAccess, FillsIndexOfNextRequestForSameKey) {
  //            idx: 0       1       2       3       4       5
  VectorReader reader({req(1), req(2), req(1), req(3), req(2), req(1)});
  const auto reqs = annotate_next_access(reader);
  EXPECT_EQ(next_of(reqs), (std::vector<std::uint64_t>{2, 4, 5, kNever, kNever, kNever}));
}

TEST(AnnotateNextAccess, KeepsOtherFields) {
  VectorReader reader({Request{.time = 7, .key = 9, .size = 123, .op = Op::Set}});
  const auto reqs = annotate_next_access(reader);
  ASSERT_EQ(reqs.size(), 1U);
  EXPECT_EQ(reqs[0].time, 7U);
  EXPECT_EQ(reqs[0].key, 9U);
  EXPECT_EQ(reqs[0].size, 123U);
  EXPECT_EQ(reqs[0].op, Op::Set);
}

TEST(AnnotateNextAccess, MaxRequestsTruncatesBeforeAnnotating) {
  // Key 1's next access (index 3) is outside the first 3 requests, so it's kNever in this run.
  VectorReader reader({req(1), req(2), req(2), req(1)});
  const auto reqs = annotate_next_access(reader, 3);
  EXPECT_EQ(next_of(reqs), (std::vector<std::uint64_t>{kNever, 2, kNever}));
}

TEST(AnnotateNextAccess, RewindsReaderFirst) {
  VectorReader reader({req(1), req(1)});
  Request r;
  ASSERT_TRUE(reader.next(r));  // consume one request
  EXPECT_EQ(annotate_next_access(reader).size(), 2U);
}

TEST(AnnotateNextAccess, EmptyTrace) {
  VectorReader reader({});
  EXPECT_TRUE(annotate_next_access(reader).empty());
}

}  // namespace
}  // namespace lcache
