#include "policy/fallback.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ml/predictor.h"
#include "policy/learned.h"
#include "policy/wtinylfu.h"
#include "reference/workloads.h"
#include "sim/cache.h"
#include "sim/simulator.h"
#include "trace/next_access.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

// A deliberately terrible model: evicts the key whose next access is the *soonest*.
class AntiOracle final : public Predictor {
 public:
  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override {
    oracle_.predict(keys, features, scores);
    for (double& s : scores) s = -s;
  }
  void observe(const Request& r) override { oracle_.observe(r); }
  bool needs_next_access() const override { return true; }

 private:
  OraclePredictor oracle_;
};

std::unique_ptr<EvictionPolicy> learned(std::unique_ptr<Predictor> p) {
  return std::make_unique<LearnedPolicy>(std::move(p), "learned");
}

std::vector<Request> zipf_trace(std::size_t n, std::uint64_t seed) {
  std::vector<Request> reqs;
  for (const Key k : test::zipf_keys(n, 2'000, 0.9, seed)) reqs.push_back(test::req(k));
  return reqs;
}

std::uint64_t hits(const std::vector<Request>& trace, EvictionPolicy& p, std::uint64_t cap) {
  VectorReader reader(trace);
  return simulate(reader, p, {.capacity_bytes = cap}).hits;
}

constexpr std::uint64_t kCap = 200;

TEST(FallbackPolicy, RejectsBadConstruction) {
  EXPECT_THROW(FallbackPolicy(nullptr, kCap), std::invalid_argument);
  EXPECT_THROW(FallbackPolicy(learned(std::make_unique<RandomPredictor>(1)), kCap, {.window = 0}),
               std::invalid_argument);
  EXPECT_THROW(
      FallbackPolicy(learned(std::make_unique<RandomPredictor>(1)), kCap, {.max_breaches = 0}),
      std::invalid_argument);
}

// ---- Hit-rate fallback -----------------------------------------------------------------------

TEST(FallbackPolicy, GoodModelNeverFallsBackAndChangesNothing) {
  const auto trace = zipf_trace(60'000, 1);
  auto alone = learned(std::make_unique<OraclePredictor>());
  FallbackPolicy guarded(learned(std::make_unique<OraclePredictor>()), kCap, {.window = 2'000});
  EXPECT_EQ(hits(trace, guarded, kCap), hits(trace, *alone, kCap));  // identical decisions
  EXPECT_EQ(guarded.stats().hit_rate_trips, 0U);
  EXPECT_EQ(guarded.stats().fallback_evictions, 0U);
  EXPECT_GT(guarded.stats().learned_evictions, 0U);
  EXPECT_EQ(guarded.mode(), FallbackPolicy::Mode::kLearned);
}

TEST(FallbackPolicy, BadModelFallsBackAndRecoversMostOfWTinyLfu) {
  const auto trace = zipf_trace(60'000, 2);
  auto bad_alone = learned(std::make_unique<AntiOracle>());
  WTinyLfuPolicy wtinylfu(kCap);
  FallbackPolicy guarded(learned(std::make_unique<AntiOracle>()), kCap,
                         {.window = 2'000, .cooldown = 10'000});
  const auto bad = hits(trace, *bad_alone, kCap);
  const auto reference = hits(trace, wtinylfu, kCap);
  const auto got = hits(trace, guarded, kCap);

  const auto& st = guarded.stats();
  EXPECT_GE(st.hit_rate_trips, 1U);
  EXPECT_GT(st.fallback_evictions, 4 * st.learned_evictions);
  EXPECT_GT(got, bad);
  EXPECT_GE(static_cast<double>(got), 0.9 * static_cast<double>(reference))
      << "bad model " << bad << ", W-TinyLFU " << reference << ", guarded " << got;
}

TEST(FallbackPolicy, ClearlyBadModelTripsBeforeAFullWindow) {
  const auto trace = zipf_trace(20'000, 6);
  FallbackPolicy guarded(learned(std::make_unique<AntiOracle>()), kCap,
                         {.window = 10'000, .cooldown = 1'000'000});
  hits(trace, guarded, kCap);
  EXPECT_EQ(guarded.stats().hit_rate_trips, 1U);
  EXPECT_EQ(guarded.stats().early_trips, 1U);
  // Tripped well before 10,000 requests: few learned evictions.
  EXPECT_LT(guarded.stats().learned_evictions, 2'000U);
}

TEST(FallbackPolicy, RetriesAfterCooldownWithBackoff) {
  const auto trace = zipf_trace(200'000, 3);
  constexpr std::uint64_t kWindow = 1'000;
  constexpr std::uint64_t kCooldown = 2'000;
  FallbackPolicy guarded(learned(std::make_unique<AntiOracle>()), kCap,
                         {.window = kWindow, .cooldown = kCooldown});
  hits(trace, guarded, kCap);
  const auto& st = guarded.stats();
  EXPECT_GE(st.retries, 2U);
  // Every retry of a bad model fails its first window, so trips = retries (+1 if it ended in
  // fallback mode).
  EXPECT_EQ(st.hit_rate_trips - st.retries, guarded.mode() == FallbackPolicy::Mode::kFallback);
  // Without backoff a retry would come every cooldown + window requests.
  EXPECT_LT(st.retries, trace.size() / (kCooldown + kWindow) / 2);
}

TEST(FallbackPolicy, ShadowAndRealHitsInTheWindowMatchIndependentReplays) {
  constexpr std::uint64_t kWindow = 3'000;
  VectorReader source(zipf_trace(20'000, 4));
  const auto trace = annotate_next_access(source);  // the oracle needs next_vtime

  // Per-request hit flags of W-TinyLFU alone and of the (never tripping) learned policy alone.
  const auto flags = [&trace](EvictionPolicy& p) {
    Cache cache(p, kCap);
    std::vector<bool> out;
    for (const Request& r : trace) out.push_back(cache.access(r));
    return out;
  };
  WTinyLfuPolicy w(kCap);
  auto alone = learned(std::make_unique<OraclePredictor>());
  const auto shadow_flags = flags(w);
  const auto real_flags = flags(*alone);

  FallbackPolicy guarded(learned(std::make_unique<OraclePredictor>()), kCap, {.window = kWindow});
  flags(guarded);
  std::uint64_t want_shadow = 0;
  std::uint64_t want_real = 0;
  for (std::size_t i = trace.size() - kWindow; i < trace.size(); ++i) {
    want_shadow += shadow_flags[i] ? 1U : 0U;
    want_real += real_flags[i] ? 1U : 0U;
  }
  EXPECT_EQ(guarded.stats().window_shadow_hits, want_shadow);
  EXPECT_EQ(guarded.stats().window_real_hits, want_real);
  EXPECT_EQ(guarded.stats().requests, trace.size());
}

TEST(FallbackPolicy, InnerPoliciesStayInSyncWhileFlapping) {
  // Variable sizes, a bad model and tiny windows: the mode switches hundreds of times. If the two
  // inner policies ever disagreed about which keys are cached, the Cache would throw (evicting an
  // untracked key) or W-TinyLFU's byte accounting would break (caught by ASan/asserts).
  std::vector<Request> trace;
  const auto keys = test::zipf_keys(50'000, 1'500, 0.8, 5);
  for (const Key k : keys) trace.push_back(test::req(k, static_cast<std::uint32_t>(1 + k % 20)));
  FallbackPolicy guarded(learned(std::make_unique<AntiOracle>()), 2'000,
                         {.window = 100, .cooldown = 100});
  VectorReader reader(trace);
  const auto r = simulate(reader, guarded, {.capacity_bytes = 2'000});
  EXPECT_EQ(r.requests, trace.size());
  EXPECT_GT(guarded.stats().hit_rate_trips, 20U);  // many switches (backoff limits them)
  EXPECT_GT(guarded.stats().learned_evictions, 0U);
  EXPECT_GT(guarded.stats().fallback_evictions, 0U);
}

// ---- Latency budget (fake clock) --------------------------------------------------------------

struct SlowModel {
  std::uint64_t now = 0;        // the fake clock, in ns
  std::uint64_t cost_ns = 100;  // what one predict() call "takes"
  FallbackOptions options(std::uint64_t budget_ns, std::uint32_t max_breaches,
                          std::uint64_t cooldown = 1'000'000) {
    return {.window = 1'000'000,  // no hit-rate trips in these tests
            .cooldown = cooldown,
            .budget_ns = budget_ns,
            .max_breaches = max_breaches,
            .clock_ns = [this] { return now; }};
  }
  std::unique_ptr<EvictionPolicy> policy() {
    return learned(std::make_unique<FunctionPredictor>([this](Key k, std::span<const float>) {
      now += cost_ns;  // simulated inference time; once per row, so k rows per eviction
      return static_cast<double>(k);
    }));
  }
};

void insert_keys(FallbackPolicy& p, Key from, Key to) {
  for (Key k = from; k < to; ++k) p.on_insert(test::req(k));
}

TEST(FallbackPolicy, LatencySpikeTripsAtOnce) {
  SlowModel m;
  FallbackPolicy p(m.policy(), kCap, m.options(/*budget_ns=*/1'000'000, /*max_breaches=*/1));
  insert_keys(p, 1, 11);

  EXPECT_EQ(p.evict(), 10U);  // fast: 10 rows x 100 ns
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kLearned);

  m.cost_ns = 500'000;       // spike: 9 rows x 0.5 ms = 4.5 ms > 1 ms
  EXPECT_EQ(p.evict(), 9U);  // this eviction still completes with the learned choice...
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kFallback);  // ...and the next ones are W-TinyLFU's
  EXPECT_EQ(p.stats().latency_trips, 1U);
  EXPECT_EQ(p.stats().budget_breaches, 1U);

  const auto calls_before = m.now;
  p.evict();
  EXPECT_EQ(m.now, calls_before);  // no inference at all while fallen back
  EXPECT_EQ(p.stats().learned_evictions, 2U);
  EXPECT_EQ(p.stats().fallback_evictions, 1U);
}

TEST(FallbackPolicy, MaxBreachesCountsSlowEvictionsSinceTheLastSwitch) {
  SlowModel m;
  FallbackPolicy p(m.policy(), kCap, m.options(1'000'000, /*max_breaches=*/3));
  insert_keys(p, 1, 21);
  m.cost_ns = 500'000;
  p.evict();  // breach 1
  p.evict();  // breach 2
  m.cost_ns = 100;
  p.evict();  // fast: doesn't reset the count
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kLearned);
  m.cost_ns = 500'000;
  p.evict();  // breach 3
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kFallback);
  EXPECT_EQ(p.stats().budget_breaches, 3U);
}

TEST(FallbackPolicy, NoBudgetMeansNoLatencyTrips) {
  SlowModel m;
  FallbackPolicy p(m.policy(), kCap, m.options(/*budget_ns=*/0, 1));
  insert_keys(p, 1, 11);
  m.cost_ns = 1'000'000'000;
  p.evict();
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kLearned);
  EXPECT_EQ(p.stats().budget_breaches, 0U);
}

TEST(FallbackPolicy, RetriesTheLearnedPolicyAfterTheCooldown) {
  SlowModel m;
  FallbackPolicy p(m.policy(), kCap, m.options(1'000'000, 1, /*cooldown=*/5));
  insert_keys(p, 1, 11);
  m.cost_ns = 500'000;
  p.evict();
  ASSERT_EQ(p.mode(), FallbackPolicy::Mode::kFallback);
  for (int i = 0; i < 4; ++i) p.on_hit(test::req(1));
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kFallback);
  p.on_hit(test::req(1));  // 5th request since the switch
  EXPECT_EQ(p.mode(), FallbackPolicy::Mode::kLearned);
  EXPECT_EQ(p.stats().retries, 1U);
}

}  // namespace
}  // namespace lcache
