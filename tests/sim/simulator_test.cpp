#include "sim/simulator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "policy/lru.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

Request req(Key k, std::uint32_t size = 1) { return Request{.key = k, .size = size}; }

// ---- SimResult arithmetic (independent of Cache::access) ----------------------------------

TEST(SimResult, RatesAreZeroForEmptyRun) {
  const SimResult r;
  EXPECT_EQ(r.hit_rate(), 0.0);
  EXPECT_EQ(r.byte_hit_rate(), 0.0);
}

TEST(SimResult, Rates) {
  const SimResult r{.requests = 4, .hits = 1, .bytes_requested = 100, .bytes_hit = 75};
  EXPECT_DOUBLE_EQ(r.hit_rate(), 0.25);
  EXPECT_DOUBLE_EQ(r.byte_hit_rate(), 0.75);
}

// ---- simulate() ---------------------------------------------------------------------------

TEST(Simulate, EmptyTrace) {
  VectorReader reader({});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 10});
  EXPECT_EQ(r.requests, 0U);
  EXPECT_EQ(r.hits, 0U);
}

TEST(Simulate, CountsHitsAndBytes) {
  // Capacity 10 bytes, all objects 4 bytes. LRU order shown oldest -> newest:
  //   1 miss [1]; 2 miss [1,2]; 1 hit [2,1]; 3 miss, 12 > 10 so evict 2 [1,3];
  //   2 miss, evict 1 [3,2]; 3 hit [2,3].  => 2 hits, 8 bytes hit.
  VectorReader reader({req(1, 4), req(2, 4), req(1, 4), req(3, 4), req(2, 4), req(3, 4)});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 10});
  EXPECT_EQ(r.requests, 6U);
  EXPECT_EQ(r.hits, 2U);
  EXPECT_EQ(r.bytes_requested, 24U);
  EXPECT_EQ(r.bytes_hit, 8U);
}

TEST(Simulate, IgnoreSizeMakesCapacityCountObjects) {
  // Objects are far larger than 2 bytes, but with ignore_size the cache holds 2 objects.
  VectorReader reader({req(1, 1000), req(2, 1000), req(1, 1000), req(3, 1000), req(1, 1000)});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 2, .ignore_size = true});
  EXPECT_EQ(r.requests, 5U);
  EXPECT_EQ(r.hits, 2U);          // the two re-accesses of key 1
  EXPECT_EQ(r.bytes_hit, 2000U);  // byte metrics still use the real sizes
  EXPECT_EQ(r.bytes_requested, 5000U);
}

TEST(Simulate, MaxRequestsStopsEarly) {
  VectorReader reader({req(1), req(1), req(1), req(1)});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 10, .max_requests = 2});
  EXPECT_EQ(r.requests, 2U);
  EXPECT_EQ(r.hits, 1U);
}

TEST(Simulate, WarmupRequestsWarmTheCacheButAreNotCounted) {
  // Capacity 1 object. Warmup: 1 miss, 1 hit. Counted: 1 hit (still cached), 2 miss, 2 hit.
  VectorReader reader({req(1, 4), req(1, 4), req(1, 4), req(2, 4), req(2, 4)});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 4, .warmup_requests = 2});
  EXPECT_EQ(r.requests, 3U);
  EXPECT_EQ(r.hits, 2U);
  EXPECT_EQ(r.bytes_requested, 12U);
  EXPECT_EQ(r.bytes_hit, 8U);
}

TEST(Simulate, MaxRequestsIncludesWarmup) {
  VectorReader reader({req(1), req(1), req(1), req(1)});
  LruPolicy lru;
  const auto r =
      simulate(reader, lru, {.capacity_bytes = 10, .max_requests = 3, .warmup_requests = 1});
  EXPECT_EQ(r.requests, 2U);
  EXPECT_EQ(r.hits, 2U);
}

TEST(Simulate, WarmupLongerThanTraceCountsNothing) {
  VectorReader reader({req(1), req(1)});
  LruPolicy lru;
  const auto r = simulate(reader, lru, {.capacity_bytes = 10, .warmup_requests = 5});
  EXPECT_EQ(r.requests, 0U);
  EXPECT_EQ(r.hit_rate(), 0.0);
}

TEST(Simulate, RewindsReaderSoItCanBeReplayed) {
  VectorReader reader({req(1), req(2), req(1)});
  LruPolicy first;
  LruPolicy second;
  const auto a = simulate(reader, first, {.capacity_bytes = 1});
  const auto b = simulate(reader, second, {.capacity_bytes = 1});
  EXPECT_EQ(a.requests, 3U);
  EXPECT_EQ(b.requests, a.requests);
  EXPECT_EQ(b.hits, a.hits);
}

}  // namespace
}  // namespace lcache

namespace lcache {
namespace {

// Records the next_vtime it is shown, to check simulate() annotates only when asked.
class NextVtimeProbe final : public EvictionPolicy {
 public:
  explicit NextVtimeProbe(bool wants) : wants_(wants) {}
  void on_hit(const Request& r) override { seen_.push_back(r.next_vtime); }
  void on_insert(const Request& r) override { seen_.push_back(r.next_vtime); }
  Key evict() override { return 0; }
  void on_remove(Key /*k*/) override {}
  std::string_view name() const override { return "probe"; }
  bool needs_next_access() const override { return wants_; }

  const std::vector<std::uint64_t>& seen() const { return seen_; }

 private:
  bool wants_;
  std::vector<std::uint64_t> seen_;
};

TEST(Simulate, AnnotatesNextAccessWhenPolicyNeedsIt) {
  VectorReader reader({req(1), req(2), req(1)});
  NextVtimeProbe probe(true);
  simulate(reader, probe, {.capacity_bytes = 10});
  EXPECT_EQ(probe.seen(), (std::vector<std::uint64_t>{2, kNever, kNever}));
}

TEST(Simulate, DoesNotAnnotateOtherwise) {
  VectorReader reader({req(1), req(2), req(1)});
  NextVtimeProbe probe(false);
  simulate(reader, probe, {.capacity_bytes = 10});
  EXPECT_EQ(probe.seen(), (std::vector<std::uint64_t>{kNever, kNever, kNever}));
}

TEST(Simulate, AnnotationRespectsMaxRequests) {
  VectorReader reader({req(1), req(2), req(2), req(1)});
  NextVtimeProbe probe(true);
  const auto r = simulate(reader, probe, {.capacity_bytes = 10, .max_requests = 3});
  EXPECT_EQ(r.requests, 3U);
  EXPECT_EQ(probe.seen(), (std::vector<std::uint64_t>{kNever, 2, kNever}));
}

}  // namespace
}  // namespace lcache
