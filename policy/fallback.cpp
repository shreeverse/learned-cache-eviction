#include "policy/fallback.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lcache {

namespace {

constexpr std::uint8_t kRealHit = 1;
constexpr std::uint8_t kShadowHit = 2;
constexpr std::uint64_t kMaxBackoff = 16;

std::uint64_t steady_now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

FallbackPolicy::FallbackPolicy(std::unique_ptr<EvictionPolicy> learned,
                               std::uint64_t capacity_bytes, FallbackOptions options)
    : learned_(std::move(learned)),
      fallback_(capacity_bytes),
      options_(std::move(options)),
      shadow_(capacity_bytes),
      capacity_(capacity_bytes) {
  if (!learned_) throw std::invalid_argument("FallbackPolicy: learned policy is null");
  if (options_.window == 0) throw std::invalid_argument("FallbackPolicy: window is 0");
  if (options_.max_breaches == 0) throw std::invalid_argument("FallbackPolicy: max_breaches is 0");
  if (!options_.clock_ns) options_.clock_ns = steady_now_ns;
  ring_.assign(options_.window, 0);
}

void FallbackPolicy::on_hit(const Request& r) {
  learned_->on_hit(r);
  fallback_.on_hit(r);
  record(r, true);
}

void FallbackPolicy::on_insert(const Request& r) {
  learned_->on_insert(r);
  fallback_.on_insert(r);
  record(r, false);
}

Key FallbackPolicy::evict() {
  if (mode_ == Mode::kFallback) {
    const Key victim = fallback_.evict();
    learned_->on_remove(victim);
    ++stats_.fallback_evictions;
    return victim;
  }

  const std::uint64_t start = options_.clock_ns();
  const Key victim = learned_->evict();
  const std::uint64_t elapsed = options_.clock_ns() - start;
  fallback_.on_remove(victim);
  ++stats_.learned_evictions;

  if (options_.budget_ns != 0 && elapsed > options_.budget_ns) {
    ++stats_.budget_breaches;
    if (++breaches_ >= options_.max_breaches) {
      ++stats_.latency_trips;
      switch_to(Mode::kFallback);  // this victim stands; the next eviction is W-TinyLFU's
    }
  }
  return victim;
}

void FallbackPolicy::on_remove(Key k) {
  learned_->on_remove(k);
  fallback_.on_remove(k);
  if (const auto it = shadow_sizes_.find(k); it != shadow_sizes_.end()) {
    shadow_used_ -= it->second;
    shadow_.on_remove(k);
    shadow_sizes_.erase(it);
  }
}

// Same rules as sim::Cache::access (decisions D1-D3).
bool FallbackPolicy::shadow_access(const Request& r) {
  if (shadow_sizes_.contains(r.key)) {
    shadow_.on_hit(r);
    return true;
  }
  if (r.size > capacity_) return false;
  shadow_sizes_.emplace(r.key, r.size);
  shadow_used_ += r.size;
  shadow_.on_insert(r);
  while (shadow_used_ > capacity_) {
    const Key victim = shadow_.evict();
    const auto it = shadow_sizes_.find(victim);
    shadow_used_ -= it->second;
    shadow_sizes_.erase(it);
  }
  return false;
}

void FallbackPolicy::record(const Request& r, bool real_hit) {
  const bool shadow_hit = shadow_access(r);

  // Slide the window: drop the oldest request's bits, add this one's.
  std::uint8_t& slot = ring_[ring_pos_];
  stats_.window_real_hits -= (slot & kRealHit) != 0 ? 1 : 0;
  stats_.window_shadow_hits -= (slot & kShadowHit) != 0 ? 1 : 0;
  slot = static_cast<std::uint8_t>((real_hit ? kRealHit : 0) | (shadow_hit ? kShadowHit : 0));
  stats_.window_real_hits += real_hit ? 1 : 0;
  stats_.window_shadow_hits += shadow_hit ? 1 : 0;
  ring_pos_ = (ring_pos_ + 1) % ring_.size();
  ++stats_.requests;
  ++since_switch_;
  real_since_ += real_hit ? 1 : 0;
  shadow_since_ += shadow_hit ? 1 : 0;

  if (mode_ == Mode::kLearned) {
    if (learned_is_behind()) {
      // A retry that fails its first window waits longer next time.
      const bool failed_first_window = since_switch_ < 2 * options_.window && stats_.retries > 0;
      backoff_ = failed_first_window ? std::min(backoff_ * 2, kMaxBackoff) : 1;
      ++stats_.hit_rate_trips;
      switch_to(Mode::kFallback);
    } else if (since_switch_ >= 2 * options_.window) {
      backoff_ = 1;  // survived two windows: the model is fine again
    }
  } else if (since_switch_ >= options_.cooldown * backoff_) {
    ++stats_.retries;
    switch_to(Mode::kLearned);
  }
}

bool FallbackPolicy::learned_is_behind() {
  if (since_switch_ >= options_.window) {
    // The rolling window now holds only learned-mode requests.
    const double allowed = options_.margin * static_cast<double>(options_.window);
    return static_cast<double>(stats_.window_real_hits) + allowed <
           static_cast<double>(stats_.window_shadow_hits);
  }
  // Early check, so a clearly bad model doesn't run for a whole window: the hit difference of two
  // policies over n requests varies by at most about 0.7 * sqrt(n), so a shortfall beyond
  // 3 * sqrt(n) is almost never noise.
  if (since_switch_ < options_.window / 10) return false;
  const auto n = static_cast<double>(since_switch_);
  const double allowed = options_.margin * n + 3.0 * std::sqrt(n);
  const bool behind =
      static_cast<double>(real_since_) + allowed < static_cast<double>(shadow_since_);
  if (behind) ++stats_.early_trips;
  return behind;
}

void FallbackPolicy::switch_to(Mode m) {
  mode_ = m;
  since_switch_ = 0;
  breaches_ = 0;
  real_since_ = 0;
  shadow_since_ = 0;
}

}  // namespace lcache
