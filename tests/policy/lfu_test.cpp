#include "policy/lfu.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include "reference/differential.h"
#include "reference/naive_lfu.h"
#include "reference/workloads.h"

namespace lcache {
namespace {

using test::req;

TEST(LfuPolicy, Name) {
  LfuPolicy lfu;
  EXPECT_EQ(lfu.name(), "lfu");
}

TEST(LfuPolicy, EvictsLowestFrequency) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_insert(req(2));
  lfu.on_hit(req(1));  // 1: freq 2, 2: freq 1
  EXPECT_EQ(lfu.evict(), 2U);
}

TEST(LfuPolicy, FrequencyBeatsRecency) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_hit(req(1));
  lfu.on_hit(req(1));  // 1: freq 3, but least recently used
  lfu.on_insert(req(2));
  lfu.on_hit(req(2));  // 2: freq 2, most recently used
  EXPECT_EQ(lfu.evict(), 2U);
  EXPECT_EQ(lfu.evict(), 1U);
}

TEST(LfuPolicy, TiesGoToWhoeverReachedTheFrequencyFirst) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_insert(req(2));
  lfu.on_insert(req(3));
  lfu.on_hit(req(2));  // 2 reaches freq 2 first
  lfu.on_hit(req(1));  // then 1
  // freq 1: [3], freq 2: [2, 1]
  EXPECT_EQ(lfu.evict(), 3U);
  EXPECT_EQ(lfu.evict(), 2U);
  EXPECT_EQ(lfu.evict(), 1U);
}

TEST(LfuPolicy, NewcomerIsNotEvictedWhileOthersExist) {
  // Under insert-then-evict the newcomer always has the lowest frequency; it must still survive,
  // otherwise LFU could never admit anything new once all residents have freq >= 2.
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_hit(req(1));     // 1: freq 2
  lfu.on_insert(req(2));  // newcomer, the only freq-1 key
  EXPECT_EQ(lfu.evict(), 1U);
}

TEST(LfuPolicy, NewcomerSurvivesSeveralEvictions) {
  // A large newcomer may need several evictions in a row (byte-sized caches).
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_insert(req(2));
  lfu.on_hit(req(2));
  lfu.on_insert(req(3));  // newcomer
  EXPECT_EQ(lfu.evict(), 1U);
  EXPECT_EQ(lfu.evict(), 2U);
  EXPECT_EQ(lfu.evict(), 3U);  // only key left
}

TEST(LfuPolicy, OlderFreqOneKeysGoBeforeNewcomer) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_insert(req(2));  // newcomer
  EXPECT_EQ(lfu.evict(), 1U);
}

TEST(LfuPolicy, FrequencyIsForgottenAfterRemoval) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_hit(req(1));  // 1: freq 2
  lfu.on_insert(req(2));
  lfu.on_hit(req(2));
  lfu.on_hit(req(2));  // 2: freq 3
  lfu.on_remove(2);
  lfu.on_insert(req(2));  // back with freq 1, not 4
  lfu.on_insert(req(3));  // newcomer
  EXPECT_EQ(lfu.evict(), 2U);
}

TEST(LfuPolicy, RemoveMiddleAndUnknownKeys) {
  LfuPolicy lfu;
  lfu.on_insert(req(1));
  lfu.on_insert(req(2));
  lfu.on_insert(req(3));
  lfu.on_remove(2);
  lfu.on_remove(99);  // not tracked: no-op
  lfu.on_hit(req(1));
  EXPECT_EQ(lfu.evict(), 3U);
  EXPECT_EQ(lfu.evict(), 1U);
}

TEST(LfuPolicy, ManyKeys) {
  constexpr Key kN = 50'000;
  LfuPolicy lfu;
  for (Key k = 0; k < kN; ++k) {
    lfu.on_insert(req(k));
    for (Key h = 0; h < k % 3; ++h) lfu.on_hit(req(k));
  }
  lfu.on_hit(req(0));  // clear newcomer protection
  // freq 1: keys = 0 mod 3 (except 0, now freq 2), in insertion order; then freq 2; then freq 3.
  Key prev_freq_class = 0;
  for (Key i = 0; i < kN; ++i) {
    const Key k = lfu.evict();
    const Key cls = (k == 0) ? 1 : k % 3;
    ASSERT_GE(cls, prev_freq_class) << "evicted " << k << " at step " << i;
    prev_freq_class = cls;
  }
}

struct DiffCase {
  std::size_t capacity;
  Key key_space;
  bool zipf;
  std::uint64_t seed;
};

void PrintTo(const DiffCase& c, std::ostream* os) {
  *os << "{capacity=" << c.capacity << ", key_space=" << c.key_space << ", zipf=" << c.zipf
      << ", seed=" << c.seed << "}";
}

class LfuDifferentialTest : public ::testing::TestWithParam<DiffCase> {};

TEST_P(LfuDifferentialTest, MatchesTextbookLfuThroughCache) {
  const auto c = GetParam();
  const auto keys = c.zipf ? test::zipf_keys(20'000, c.key_space, 0.9, c.seed)
                           : test::uniform_keys(20'000, c.key_space, c.seed);
  LfuPolicy lfu;
  test::NaiveLfu ref(c.capacity);
  test::expect_matches_reference(lfu, ref, c.capacity, keys, c.key_space);
}

INSTANTIATE_TEST_SUITE_P(Cases, LfuDifferentialTest,
                         ::testing::Values(DiffCase{1, 4, false, 1}, DiffCase{3, 8, false, 2},
                                           DiffCase{10, 40, true, 3}, DiffCase{50, 200, true, 4},
                                           DiffCase{50, 60, false, 5}),
                         [](const ::testing::TestParamInfo<DiffCase>& param_info) {
                           return "cap" + std::to_string(param_info.param.capacity) + "_keys" +
                                  std::to_string(param_info.param.key_space) +
                                  (param_info.param.zipf ? "_zipf" : "_uniform");
                         });

}  // namespace
}  // namespace lcache
