#include "policy/lru.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <tuple>
#include <unordered_set>

#include "reference/naive_lru.h"

namespace lcache {
namespace {

Request req(Key k, std::uint32_t size = 1) { return Request{.key = k, .size = size}; }

// ---- Hand-built sequences ------------------------------------------------------------------

TEST(LruPolicy, Name) {
  LruPolicy lru;
  EXPECT_EQ(lru.name(), "lru");
}

TEST(LruPolicy, EvictsInInsertionOrderWithoutHits) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  lru.on_insert(req(3));
  EXPECT_EQ(lru.evict(), 1U);
  EXPECT_EQ(lru.evict(), 2U);
  EXPECT_EQ(lru.evict(), 3U);
}

TEST(LruPolicy, HitMovesKeyToMostRecent) {
  // Classic A B C A D with room for 3: the hit on A makes B the victim.
  LruPolicy lru;
  lru.on_insert(req(1));  // A
  lru.on_insert(req(2));  // B
  lru.on_insert(req(3));  // C
  lru.on_hit(req(1));     // A
  lru.on_insert(req(4));  // D
  EXPECT_EQ(lru.evict(), 2U);
  EXPECT_EQ(lru.evict(), 3U);
  EXPECT_EQ(lru.evict(), 1U);
  EXPECT_EQ(lru.evict(), 4U);
}

TEST(LruPolicy, RepeatedHitsOnMostRecentKeyChangeNothing) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  lru.on_hit(req(2));
  lru.on_hit(req(2));
  EXPECT_EQ(lru.evict(), 1U);
  EXPECT_EQ(lru.evict(), 2U);
}

TEST(LruPolicy, CanEvictTheKeyJustInserted) {
  // With a single tracked key, the newcomer is also the LRU key (decisions.md D2).
  LruPolicy lru;
  lru.on_insert(req(7));
  EXPECT_EQ(lru.evict(), 7U);
}

TEST(LruPolicy, SizeDoesNotAffectOrder) {
  LruPolicy lru;
  lru.on_insert(req(1, 1'000'000));
  lru.on_insert(req(2, 1));
  EXPECT_EQ(lru.evict(), 1U);
}

TEST(LruPolicy, RemoveOldestKey) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  lru.on_insert(req(3));
  lru.on_remove(1);
  EXPECT_EQ(lru.evict(), 2U);
  EXPECT_EQ(lru.evict(), 3U);
}

TEST(LruPolicy, RemoveMiddleKey) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  lru.on_insert(req(3));
  lru.on_remove(2);
  EXPECT_EQ(lru.evict(), 1U);
  EXPECT_EQ(lru.evict(), 3U);
}

TEST(LruPolicy, RemoveUnknownKeyIsNoOp) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_remove(99);
  EXPECT_EQ(lru.evict(), 1U);
}

TEST(LruPolicy, KeyCanBeReinsertedAfterEviction) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  EXPECT_EQ(lru.evict(), 1U);
  lru.on_insert(req(1));  // 1 is now the most recent
  EXPECT_EQ(lru.evict(), 2U);
  EXPECT_EQ(lru.evict(), 1U);
}

TEST(LruPolicy, KeyCanBeReinsertedAfterRemove) {
  LruPolicy lru;
  lru.on_insert(req(1));
  lru.on_insert(req(2));
  lru.on_remove(1);
  lru.on_insert(req(1));
  EXPECT_EQ(lru.evict(), 2U);
  EXPECT_EQ(lru.evict(), 1U);
}

TEST(LruPolicy, ManyKeysEvictInOrder) {
  constexpr Key kN = 100'000;
  LruPolicy lru;
  for (Key k = 0; k < kN; ++k) lru.on_insert(req(k));
  for (Key k = 0; k < kN; ++k) ASSERT_EQ(lru.evict(), k);
}

// ---- Differential test against a naive reference ------------------------------------------
//
// Drives LruPolicy the way a unit-size cache of `capacity` objects would (insert on miss, evict
// when over capacity, occasional explicit removes) and checks every hit/miss and every evicted
// key against NaiveLru.

struct DiffParams {
  std::size_t capacity;
  Key key_space;
  std::uint64_t seed;
};

// Lets GoogleTest print parameters readably instead of as a raw byte dump.
void PrintTo(const DiffParams& p, std::ostream* os) {
  *os << "{capacity=" << p.capacity << ", key_space=" << p.key_space << ", seed=" << p.seed << "}";
}

class LruDifferentialTest : public ::testing::TestWithParam<DiffParams> {};

TEST_P(LruDifferentialTest, MatchesNaiveLru) {
  const auto [capacity, key_space, seed] = GetParam();
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<Key> pick_key(0, key_space - 1);
  std::bernoulli_distribution do_remove(0.05);

  LruPolicy lru;
  test::NaiveLru ref(capacity);
  std::unordered_set<Key> cached;  // what a Cache would hold

  for (int i = 0; i < 20'000; ++i) {
    const Key k = pick_key(rng);
    SCOPED_TRACE(::testing::Message() << "step " << i << ", key " << k);

    if (do_remove(rng)) {
      lru.on_remove(k);  // also exercises removing absent keys
      ref.remove(k);
      cached.erase(k);
      continue;
    }

    const auto expected = ref.access(k);
    const bool hit = cached.contains(k);
    ASSERT_EQ(hit, expected.hit);

    std::optional<Key> evicted;
    if (hit) {
      lru.on_hit(req(k));
    } else {
      lru.on_insert(req(k));
      cached.insert(k);
      if (cached.size() > capacity) {
        evicted = lru.evict();
        cached.erase(*evicted);
      }
    }
    ASSERT_EQ(evicted, expected.evicted);
  }
}

INSTANTIATE_TEST_SUITE_P(Params, LruDifferentialTest,
                         ::testing::Values(DiffParams{1, 4, 1}, DiffParams{3, 6, 2},
                                           DiffParams{10, 25, 3}, DiffParams{64, 100, 4},
                                           DiffParams{64, 1000, 5}),
                         [](const ::testing::TestParamInfo<DiffParams>& param_info) {
                           return "cap" + std::to_string(param_info.param.capacity) + "_keys" +
                                  std::to_string(param_info.param.key_space);
                         });

}  // namespace
}  // namespace lcache
