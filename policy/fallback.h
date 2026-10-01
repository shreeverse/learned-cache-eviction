#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "policy/eviction_policy.h"
#include "policy/wtinylfu.h"

namespace lcache {

struct FallbackOptions {
  // Requests in the rolling window over which the learned policy's real hits are compared with a
  // shadow W-TinyLFU's.
  std::uint64_t window = 10'000;
  // Fall back when real hits < shadow hits - margin * window (0 = any shortfall).
  double margin = 0.0;
  // Requests after each switch to the learned policy (and at the start) during which it isn't
  // judged: the cache it inherits was shaped by W-TinyLFU, and reshaping it takes time. 30k was
  // chosen on dev splits over 0 / 10k (docs/m4-results.md).
  std::uint64_t grace = 30'000;
  // Also judge early (from grace + window/10), tripping on a shortfall beyond 3 * sqrt(n).
  bool early_check = true;
  // Requests to stay on W-TinyLFU before retrying the learned policy. Doubles (up to 16x) each
  // time a retry fails its first window, and resets once a retry survives two windows.
  std::uint64_t cooldown = 50'000;
  // Time budget per learned eviction, in nanoseconds; 0 = no budget.
  std::uint64_t budget_ns = 0;
  // Learned evictions over budget within the last `breach_window` learned evictions that trip the
  // fallback; 1 = at the first breach. Counting in a window (not since the last switch) keeps
  // rare scheduling jitter from adding up to a trip over a long run.
  std::uint32_t max_breaches = 1;
  std::uint64_t breach_window = 1'000;
  // Monotonic clock in nanoseconds; defaults to std::chrono::steady_clock. Tests inject a fake.
  std::function<std::uint64_t()> clock_ns{};
};

// Defensive wrapper around a learned policy (M4, docs/decisions.md D21). Routes each eviction to
// the learned policy or to W-TinyLFU:
//
//   learned --(after a grace period: over the last `window` requests, real hits < shadow
//              W-TinyLFU hits - margin; or early, from window/10 requests into judging, if
//              behind by more than margin * n + 3 * sqrt(n); or, at any time, `max_breaches`
//              evictions over the time budget) --> fallback
//   fallback --(after `cooldown` requests)--> learned (retry; judged again after `window`)
//
// Both inner policies see every hit and insert, and each is told about the other's victims
// (on_remove), so both always track exactly the cache's keys and switching is instant. The
// shadow W-TinyLFU runs on its own key -> size bookkeeping (the same rules as sim::Cache, which
// this library can't depend on) and sees every request, so it is the counterfactual "what if we
// had used W-TinyLFU all along".
//
// A breach is detected after the slow eviction completes (inference isn't interruptible); the
// switch takes effect from the next eviction.
class FallbackPolicy final : public EvictionPolicy {
 public:
  enum class Mode : std::uint8_t { kLearned, kFallback };

  struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t learned_evictions = 0;
    std::uint64_t fallback_evictions = 0;
    std::uint64_t hit_rate_trips = 0;  // includes early trips
    std::uint64_t early_trips = 0;
    std::uint64_t latency_trips = 0;
    std::uint64_t budget_breaches = 0;  // all learned evictions over budget
    std::uint64_t retries = 0;
    std::uint64_t window_real_hits = 0;    // in the current rolling window
    std::uint64_t window_shadow_hits = 0;  // in the current rolling window
  };

  // Throws std::invalid_argument if learned is null, window is 0 or max_breaches is 0.
  FallbackPolicy(std::unique_ptr<EvictionPolicy> learned, std::uint64_t capacity_bytes,
                 FallbackOptions options = {});

  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "guarded"; }
  bool needs_next_access() const override { return learned_->needs_next_access(); }

  Mode mode() const { return mode_; }
  const Stats& stats() const { return stats_; }

 private:
  void record(const Request& r, bool real_hit);
  bool shadow_access(const Request& r);
  bool learned_is_behind();  // the hit-rate trip condition (counts early trips)
  void switch_to(Mode m);

  std::unique_ptr<EvictionPolicy> learned_;
  WTinyLfuPolicy fallback_;
  FallbackOptions options_;

  // Shadow cache: W-TinyLFU on its own byte accounting.
  WTinyLfuPolicy shadow_;
  std::uint64_t capacity_;
  std::uint64_t shadow_used_ = 0;
  std::unordered_map<Key, std::uint32_t> shadow_sizes_;

  // Rolling window: per request, bit 0 = real hit, bit 1 = shadow hit.
  std::vector<std::uint8_t> ring_;
  std::size_t ring_pos_ = 0;

  Mode mode_ = Mode::kLearned;
  std::uint64_t since_switch_ = 0;      // requests since the last mode switch
  std::deque<std::uint64_t> breaches_;  // learned-eviction numbers of recent budget breaches
  std::uint64_t real_since_ = 0;        // real hits since judging started (after the grace period)
  std::uint64_t shadow_since_ = 0;      // shadow hits since judging started
  std::uint64_t backoff_ = 1;           // cooldown multiplier
  Stats stats_;
};

}  // namespace lcache
