#include "policy/belady.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "policy/policy_factory.h"
#include "reference/exhaustive_opt.h"
#include "reference/workloads.h"
#include "sim/simulator.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

Request at(Key k, std::uint64_t next) { return Request{.key = k, .size = 1, .next_vtime = next}; }

std::vector<Request> unit_requests(const std::vector<Key>& keys) {
  std::vector<Request> reqs;
  reqs.reserve(keys.size());
  for (const Key k : keys) reqs.push_back(test::req(k));
  return reqs;
}

std::uint64_t belady_hits(const std::vector<Key>& keys, std::uint64_t capacity) {
  VectorReader reader(unit_requests(keys));
  BeladyPolicy belady;
  return simulate(reader, belady, {.capacity_bytes = capacity}).hits;
}

TEST(BeladyPolicy, NameAndNeedsFuture) {
  const BeladyPolicy b;
  EXPECT_EQ(b.name(), "belady");
  EXPECT_TRUE(b.needs_next_access());
}

TEST(BeladyPolicy, EvictsFurthestNextAccess) {
  BeladyPolicy b;
  b.on_insert(at(1, 10));
  b.on_insert(at(2, 5));
  b.on_insert(at(3, 20));
  EXPECT_EQ(b.evict(), 3U);
  EXPECT_EQ(b.evict(), 1U);
  EXPECT_EQ(b.evict(), 2U);
}

TEST(BeladyPolicy, NeverAccessedAgainGoesFirst) {
  BeladyPolicy b;
  b.on_insert(at(1, 1'000'000));
  b.on_insert(at(2, kNever));
  EXPECT_EQ(b.evict(), 2U);
}

TEST(BeladyPolicy, HitUpdatesNextAccess) {
  BeladyPolicy b;
  b.on_insert(at(1, 3));
  b.on_insert(at(2, 7));
  b.on_hit(at(1, 9));  // key 1's next use is now further away than key 2's
  EXPECT_EQ(b.evict(), 1U);
}

TEST(BeladyPolicy, RemoveKnownAndUnknownKeys) {
  BeladyPolicy b;
  b.on_insert(at(1, 3));
  b.on_insert(at(2, 7));
  b.on_remove(2);
  b.on_remove(99);
  EXPECT_EQ(b.evict(), 1U);
}

TEST(BeladyPolicy, SkipsANewcomerItWillNeverReuse) {
  // Capacity 1, trace 1 2 1: keeping 1 and skipping 2 gets one hit; LRU gets none.
  EXPECT_EQ(belady_hits({1, 2, 1}, 1), 1U);
}

TEST(BeladyPolicy, AnnotatesThroughTheSimulator) {
  // Indices 0..4: 1 2 3 1 2, room for 2. Next uses: 1 -> 3, 2 -> 4, 3 -> never. When 3 arrives it
  // has the furthest next use, so Belady skips it and then hits on both 1 and 2.
  EXPECT_EQ(belady_hits({1, 2, 3, 1, 2}, 2), 2U);
}

// ---- Optimality: Belady equals the exhaustive optimum on small random traces -----------------

TEST(BeladyPolicy, MatchesExhaustiveOptimumOnSmallTraces) {
  int checked = 0;
  for (std::uint64_t seed = 0; seed < 300; ++seed) {
    const std::size_t capacity = 1 + seed % 3;
    const Key key_space = 3 + seed % 4;
    const auto keys = (seed % 2 == 0) ? test::uniform_keys(16, key_space, seed)
                                      : test::zipf_keys(16, key_space, 0.8, seed);
    test::ExhaustiveOpt opt(keys, capacity);
    ASSERT_EQ(belady_hits(keys, capacity), opt.max_hits())
        << "seed " << seed << ", capacity " << capacity << ", key space " << key_space;
    ++checked;
  }
  EXPECT_EQ(checked, 300);
}

TEST(BeladyPolicy, IsAnUpperBoundForTheOtherPolicies) {
  const auto keys = test::zipf_keys(50'000, 2000, 0.9, 31);
  constexpr std::uint64_t kCap = 200;
  VectorReader reader(unit_requests(keys));
  BeladyPolicy belady;
  const auto best = simulate(reader, belady, {.capacity_bytes = kCap}).hits;
  for (const std::string& name : policy_names()) {
    if (name == "belady") continue;
    const auto policy = make_policy(name, kCap);
    EXPECT_LE(simulate(reader, *policy, {.capacity_bytes = kCap}).hits, best) << name;
  }
}

}  // namespace
}  // namespace lcache
