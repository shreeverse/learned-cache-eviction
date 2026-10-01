#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <span>
#include <string_view>
#include <unordered_map>

#include "trace/request.h"

namespace lcache {

// Per-key access history and the feature vector computed from it (docs/m3-design.md section 1).
// Used by both the learned policy and the training-data collector, so training and inference
// features are computed by the same code.
//
// Time is virtual (decisions D11): the i-th call to on_access (0-based) happens at time i.
//
// Memory window (decisions D17, revising D13): forget() keeps the history of the most recently
// forgotten keys ("ghosts"), so a key that comes back continues its real history instead of
// starting over. Either a fixed number of ghosts, or (D20) a multiple of the number of tracked
// keys, so the window scales with the cache.
//
// Normalized features (D20): age and gaps divided by the number of tracked keys (= objects in the
// cache), so the same model sees comparable values at different cache sizes. Normalized columns
// have different names (feature_names(true)), so a model can't be used with the wrong kind.
struct FeatureConfig {
  std::size_t history = 0;   // fixed ghost count (used when history_ratio is 0)
  double history_ratio = 0;  // if > 0: at most ceil(ratio * tracked keys) ghosts
  bool normalize = false;    // age and gaps / tracked keys

  friend bool operator==(const FeatureConfig&, const FeatureConfig&) = default;
};

class FeatureTracker {
 public:
  explicit FeatureTracker(FeatureConfig config = {}) : config_(config) {}
  explicit FeatureTracker(std::size_t history_capacity)
      : FeatureTracker(FeatureConfig{.history = history_capacity}) {}

  static constexpr std::size_t kNumDeltas = 8;
  static constexpr std::size_t kNumEdcs = 4;
  static constexpr std::size_t kNumFeatures = 2 + kNumDeltas + 1 + kNumEdcs;

  // Column layout of a feature row.
  static constexpr std::size_t kLogSize = 0;
  static constexpr std::size_t kAge = 1;
  static constexpr std::size_t kDelta0 = 2;  // delta_1 (most recent gap) .. delta_8
  static constexpr std::size_t kCount = kDelta0 + kNumDeltas;
  static constexpr std::size_t kEdc0 = kCount + 1;  // edc_1 .. edc_4

  // Half-lives of the decayed counters, in requests. Spread out so the model sees popularity at
  // several time scales.
  static constexpr std::array<double, kNumEdcs> kEdcHalfLives{512.0, 2048.0, 8192.0, 32768.0};

  // Records an access to k at time now() + 1 (or 0 for the first access overall), and tracks k
  // (a ghost becomes tracked again). The size is taken from the key's first access only, like
  // sim::Cache (decisions D3).
  void on_access(Key k, std::uint32_t size);

  // Stops tracking k: keeps its history as a ghost (dropping the oldest ghost if over capacity),
  // or drops it if the capacity is 0. A no-op if k is not tracked.
  void forget(Key k);

  // Tracked keys (not ghosts).
  bool contains(Key k) const;
  std::size_t size() const { return history_.size() - ghosts_.size(); }

  // Forgotten keys whose history is still kept.
  bool remembers(Key k) const;
  std::size_t ghost_count() const { return ghosts_.size(); }
  // Ghosts kept at most, right now (a multiple of size() with history_ratio).
  std::size_t history_capacity() const;
  const FeatureConfig& config() const { return config_; }

  // Time of the most recent access (0 before any access).
  std::uint64_t now() const { return accesses_ == 0 ? 0 : accesses_ - 1; }

  // Writes k's features as of now() into out (out.size() must be kNumFeatures). Missing gaps
  // are NaN. Throws std::out_of_range if k has no history (tracked or ghost).
  void write_features(Key k, std::span<float> out) const;

  // Column names, in row order. Also the names the trained model must have (checked at load).
  static std::span<const std::string_view, kNumFeatures> feature_names(bool normalized = false);
  std::span<const std::string_view, kNumFeatures> names() const {
    return feature_names(config_.normalize);
  }

 private:
  struct KeyHistory {
    std::uint64_t last_vtime = 0;
    std::uint32_t size = 0;
    std::uint32_t count = 0;
    std::array<std::uint32_t, kNumDeltas> deltas{};  // ring buffer; delta_head = next write slot
    std::array<float, kNumEdcs> edc{};
    std::uint8_t delta_head = 0;
    std::uint8_t delta_len = 0;
  };

  struct Entry {
    KeyHistory h;
    bool ghost = false;
    std::list<Key>::iterator ghost_pos;  // valid only if ghost
  };

  FeatureConfig config_;
  std::uint64_t accesses_ = 0;
  std::unordered_map<Key, Entry> history_;  // tracked keys and ghosts
  std::list<Key> ghosts_;                   // oldest forgotten first
};

}  // namespace lcache
