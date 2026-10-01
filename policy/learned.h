#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ml/feature_tracker.h"
#include "ml/predictor.h"
#include "policy/eviction_policy.h"

namespace lcache {

struct LearnedOptions {
  // Candidates scored per eviction. If at most this many keys are cached, all of them are.
  std::uint32_t sample_size = 64;
  std::uint64_t seed = 1;  // for candidate sampling; runs are deterministic for a given seed
};

// Learned eviction in the style of Learning Relaxed Belady (docs/m3-design.md section 3): on
// evict(), sample candidates uniformly from the cached keys, compute their features, score them
// with one Predictor call, and evict the highest score (the furthest predicted next access).
// The newcomer can be sampled, so it may be the victim (decisions D2).
//
// The predictor decides what this policy is: a LightGBM model ("learned"), the true future
// ("oracle-sampled" = sampled Belady), or random scores ("random-sampled").
//
// Costs: O(1) per hit/insert; O(k * F) per eviction plus one predict() call.
class LearnedPolicy final : public EvictionPolicy {
 public:
  // Throws std::invalid_argument if predictor is null or sample_size is 0.
  LearnedPolicy(std::unique_ptr<Predictor> predictor, std::string name,
                LearnedOptions options = {});

  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return name_; }
  bool needs_next_access() const override { return predictor_->needs_next_access(); }

  const FeatureTracker& tracker() const { return tracker_; }

 private:
  void untrack(Key k);  // a no-op if k is not tracked

  std::unique_ptr<Predictor> predictor_;
  std::string name_;
  LearnedOptions options_;
  FeatureTracker tracker_;
  std::mt19937_64 rng_;

  std::vector<Key> keys_;                     // cached keys, in no particular order
  std::unordered_map<Key, std::size_t> pos_;  // key -> its index in keys_

  // Per-eviction buffers, kept to avoid allocating on every eviction.
  std::vector<Key> candidates_;
  std::vector<float> features_;
  std::vector<double> scores_;
};

}  // namespace lcache
