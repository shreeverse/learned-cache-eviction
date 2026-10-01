#include "policy/arc.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "policy/lru.h"
#include "reference/paper_arc.h"
#include "reference/workloads.h"
#include "sim/cache.h"

namespace lcache {
namespace {

using test::req;

TEST(ArcPolicy, Name) {
  ArcPolicy arc(10);
  EXPECT_EQ(arc.name(), "arc");
}

TEST(ArcPolicy, NewKeysEnterT1AndASecondAccessMovesThemToT2) {
  ArcPolicy arc(10);
  Cache cache(arc, 10);
  cache.access(req(1));
  cache.access(req(2));
  EXPECT_EQ(arc.state().t1, 2U);
  EXPECT_EQ(arc.state().t2, 0U);
  EXPECT_TRUE(cache.access(req(1)));
  EXPECT_EQ(arc.state().t1, 1U);
  EXPECT_EQ(arc.state().t2, 1U);
}

// Hand-derived from the paper's cases with c = 2 (unit sizes). Lists written LRU -> MRU.
TEST(ArcPolicy, WalkthroughMatchesThePaper) {
  ArcPolicy arc(2);
  Cache cache(arc, 2);
  const auto expect = [&](const std::vector<Key>& cached, const std::vector<Key>& absent, double p,
                          std::uint64_t t1, std::uint64_t t2, std::uint64_t b1, std::uint64_t b2) {
    for (Key k : cached) EXPECT_TRUE(cache.contains(k)) << "key " << k;
    for (Key k : absent) EXPECT_FALSE(cache.contains(k)) << "key " << k;
    const auto s = arc.state();
    EXPECT_DOUBLE_EQ(s.p, p);
    EXPECT_EQ(s.t1, t1);
    EXPECT_EQ(s.t2, t2);
    EXPECT_EQ(s.b1, b1);
    EXPECT_EQ(s.b2, b2);
  };

  cache.access(req(1));  // IV: T1=[1]
  cache.access(req(1));  // I:  T2=[1]
  cache.access(req(2));  // IV: T1=[2] T2=[1]
  {
    SCOPED_TRACE("3: case IV-B, REPLACE moves T1's LRU (2) to B1");
    EXPECT_FALSE(cache.access(req(3)));  // T1=[3] T2=[1] B1=[2]
    expect({1, 3}, {2}, 0.0, 1, 1, 1, 0);
  }
  {
    SCOPED_TRACE("2: B1 ghost hit, p 0 -> 1, |T1| = p so REPLACE takes T2's LRU (1)");
    EXPECT_FALSE(cache.access(req(2)));  // T1=[3] T2=[2] B2=[1]
    expect({2, 3}, {1}, 1.0, 1, 1, 0, 1);
  }
  {
    SCOPED_TRACE("1: B2 ghost hit, p 1 -> 0, |T1| > p so REPLACE takes T1's LRU (3)");
    EXPECT_FALSE(cache.access(req(1)));  // T2=[2,1] B1=[3]
    expect({1, 2}, {3}, 0.0, 0, 2, 1, 0);
  }
  {
    SCOPED_TRACE("4: case IV-B with empty T1, REPLACE takes T2's LRU (2)");
    EXPECT_FALSE(cache.access(req(4)));  // T1=[4] T2=[1] B1=[3] B2=[2]
    expect({1, 4}, {2}, 0.0, 1, 1, 1, 1);
  }
  {
    SCOPED_TRACE("3: B1 ghost hit, p 0 -> 1, REPLACE takes T2's LRU (1)");
    EXPECT_FALSE(cache.access(req(3)));  // T1=[4] T2=[3] B2=[2,1]
    expect({3, 4}, {1}, 1.0, 1, 1, 0, 2);
  }
}

TEST(ArcPolicy, CaseIVAWithFullT1DropsLruWithoutGhost) {
  // |T1| = c and B1 empty: the paper deletes T1's LRU outright (no ghost).
  ArcPolicy arc(2);
  Cache cache(arc, 2);
  cache.access(req(1));
  cache.access(req(2));
  cache.access(req(3));
  EXPECT_FALSE(cache.contains(1));
  EXPECT_EQ(arc.state().b1, 0U);
  EXPECT_EQ(arc.state().t1, 2U);
}

TEST(ArcPolicy, ResistsScansThatWouldFlushLru) {
  // Five hot keys are seen twice (-> T2), then a long one-time scan. ARC keeps evicting from T1, so
  // the hot keys survive; LRU loses all of them.
  const auto run = [](EvictionPolicy& policy) {
    Cache cache(policy, 10);
    for (int round = 0; round < 2; ++round) {
      for (Key k = 0; k < 5; ++k) cache.access(req(k));
    }
    for (Key k = 1000; k < 2000; ++k) cache.access(req(k));
    int hits = 0;
    for (Key k = 0; k < 5; ++k) hits += cache.access(req(k)) ? 1 : 0;
    return hits;
  };
  ArcPolicy arc(10);
  LruPolicy lru;
  EXPECT_EQ(run(arc), 5);
  EXPECT_EQ(run(lru), 0);
}

TEST(ArcPolicy, RemoveResidentAndUnknownKeys) {
  ArcPolicy arc(10);
  arc.on_insert(req(1));
  arc.on_insert(req(2));
  arc.on_hit(req(2));  // T1=[1] T2=[2]
  arc.on_remove(1);
  arc.on_remove(99);
  EXPECT_EQ(arc.state().t1, 0U);
  EXPECT_EQ(arc.state().t2, 1U);
  EXPECT_EQ(arc.state().b1, 0U);  // removed, not ghosted
  EXPECT_EQ(arc.evict(), 2U);
}

TEST(ArcPolicy, ByteInvariantsHoldWithVariableSizes) {
  std::mt19937_64 rng(11);
  std::uniform_int_distribution<std::uint32_t> pick_size(1, 300);
  const auto keys = test::zipf_keys(30'000, 400, 0.8, 12);
  constexpr std::uint64_t kCap = 2000;
  ArcPolicy arc(kCap);
  Cache cache(arc, kCap);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    cache.access(req(keys[i], pick_size(rng)));
    const auto s = arc.state();
    ASSERT_EQ(s.t1 + s.t2, cache.used_bytes()) << "step " << i;
    ASSERT_LE(s.t1 + s.b1, kCap) << "step " << i;
    ASSERT_LE(s.t1 + s.t2 + s.b1 + s.b2, 2 * kCap) << "step " << i;
    ASSERT_GE(s.p, 0.0) << "step " << i;
    ASSERT_LE(s.p, static_cast<double>(kCap)) << "step " << i;
  }
}

struct DiffCase {
  std::size_t capacity;
  Key key_space;
  double zipf_alpha;  // 0 = uniform
  std::uint64_t seed;
};

void PrintTo(const DiffCase& c, std::ostream* os) {
  *os << "{capacity=" << c.capacity << ", key_space=" << c.key_space
      << ", zipf_alpha=" << c.zipf_alpha << ", seed=" << c.seed << "}";
}

class ArcDifferentialTest : public ::testing::TestWithParam<DiffCase> {};

// Through the real Cache, our ARC must match the paper transcription step for step: same hit/miss,
// same contents and the same p after every request.
TEST_P(ArcDifferentialTest, MatchesPaperPseudocode) {
  const auto c = GetParam();
  const auto keys = c.zipf_alpha > 0 ? test::zipf_keys(20'000, c.key_space, c.zipf_alpha, c.seed)
                                     : test::uniform_keys(20'000, c.key_space, c.seed);
  ArcPolicy arc(c.capacity);
  Cache cache(arc, c.capacity);
  test::PaperArc ref(c.capacity);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const Key k = keys[i];
    ASSERT_EQ(cache.access(req(k)), ref.access(k)) << "hit/miss differs at step " << i;
    ASSERT_DOUBLE_EQ(arc.state().p, ref.p()) << "p differs at step " << i;
    for (Key q = 0; q < c.key_space; ++q) {
      ASSERT_EQ(cache.contains(q), ref.contains(q))
          << "contents differ at step " << i << " key " << q;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Cases, ArcDifferentialTest,
                         ::testing::Values(DiffCase{1, 4, 0, 1}, DiffCase{2, 6, 0, 2},
                                           DiffCase{3, 10, 0.7, 3}, DiffCase{10, 40, 0.8, 4},
                                           DiffCase{10, 15, 0, 5}, DiffCase{50, 200, 0.9, 6},
                                           DiffCase{50, 500, 1.1, 7}),
                         [](const ::testing::TestParamInfo<DiffCase>& param_info) {
                           return "cap" + std::to_string(param_info.param.capacity) + "_keys" +
                                  std::to_string(param_info.param.key_space) + "_seed" +
                                  std::to_string(param_info.param.seed);
                         });

}  // namespace
}  // namespace lcache
