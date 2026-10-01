#include "apps/cachesim/size_parse.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace lcache {
namespace {

TEST(ParseSize, PlainBytes) {
  EXPECT_EQ(parse_size("0"), 0U);
  EXPECT_EQ(parse_size("1048576"), 1048576U);
}

TEST(ParseSize, BinarySuffixes) {
  EXPECT_EQ(parse_size("1KiB"), 1024U);
  EXPECT_EQ(parse_size("64MiB"), 64ULL << 20);
  EXPECT_EQ(parse_size("2GiB"), 2ULL << 30);
  EXPECT_EQ(parse_size("1TiB"), 1ULL << 40);
}

TEST(ParseSize, RejectsBadInput) {
  for (const char* bad : {"", "MiB", "-1", "1.5GiB", "10MB", "10 MiB", "10mib", "1x",
                          "99999999999999999999", "16777216TiB"}) {
    SCOPED_TRACE(bad);
    EXPECT_THROW(parse_size(bad), std::invalid_argument);
  }
}

}  // namespace
}  // namespace lcache
