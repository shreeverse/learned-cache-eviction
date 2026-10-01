#include "policy/learned.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace lcache {

namespace {

// A NaN score (a broken model) must never win, so it ranks below every number.
double rank(double score) {
  return std::isnan(score) ? -std::numeric_limits<double>::infinity() : score;
}

}  // namespace

LearnedPolicy::LearnedPolicy(std::unique_ptr<Predictor> predictor, std::string name,
                             LearnedOptions options)
    : predictor_(std::move(predictor)),
      name_(std::move(name)),
      options_(options),
      rng_(options.seed) {
  if (!predictor_) throw std::invalid_argument("LearnedPolicy: predictor is null");
  if (options_.sample_size == 0) throw std::invalid_argument("LearnedPolicy: sample_size is 0");
}

void LearnedPolicy::on_hit(const Request& r) {
  predictor_->observe(r);
  tracker_.on_access(r.key, r.size);
}

void LearnedPolicy::on_insert(const Request& r) {
  assert(!pos_.contains(r.key) && "on_insert: key already tracked");
  predictor_->observe(r);
  tracker_.on_access(r.key, r.size);
  pos_.emplace(r.key, keys_.size());
  keys_.push_back(r.key);
}

Key LearnedPolicy::evict() {
  assert(!keys_.empty() && "evict: nothing tracked");
  constexpr std::size_t kF = FeatureTracker::kNumFeatures;

  candidates_.clear();
  if (keys_.size() <= options_.sample_size) {
    candidates_.assign(keys_.begin(), keys_.end());
  } else {
    // Uniform with replacement. Modulo bias is at most n / 2^64, negligible, and unlike
    // std::uniform_int_distribution the result is the same on every standard library.
    for (std::uint32_t i = 0; i < options_.sample_size; ++i) {
      candidates_.push_back(keys_[rng_() % keys_.size()]);
    }
  }

  const std::size_t n = candidates_.size();
  features_.resize(n * kF);
  scores_.resize(n);
  const std::span<float> rows(features_);
  for (std::size_t i = 0; i < n; ++i)
    tracker_.write_features(candidates_[i], rows.subspan(i * kF, kF));
  predictor_->predict(candidates_, features_, scores_);

  std::size_t best = 0;
  for (std::size_t i = 1; i < n; ++i) {
    const double a = rank(scores_[i]);
    const double b = rank(scores_[best]);
    if (a > b || (a == b && candidates_[i] < candidates_[best])) best = i;
  }
  const Key victim = candidates_[best];
  untrack(victim);
  return victim;
}

void LearnedPolicy::on_remove(Key k) {
  if (pos_.contains(k)) untrack(k);
}

void LearnedPolicy::untrack(Key k) {
  // Swap-remove: move the last key into k's slot so keys_ stays dense for O(1) sampling.
  const auto it = pos_.find(k);
  const std::size_t i = it->second;
  const Key moved = keys_.back();
  keys_[i] = moved;
  pos_[moved] = i;
  keys_.pop_back();
  pos_.erase(it);
  tracker_.forget(k);
}

}  // namespace lcache
