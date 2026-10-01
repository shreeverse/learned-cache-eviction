#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
// History is kept only for keys the owner tracks; forget() drops it (D13).
class FeatureTracker {
 public:
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

  // Records an access to k at time now() + 1 (or 0 for the first access overall). The size is
  // taken from the key's first access only, like sim::Cache (decisions D3).
  void on_access(Key k, std::uint32_t size);

  // Drops k's history. A no-op if k is unknown.
  void forget(Key k) { history_.erase(k); }

  bool contains(Key k) const { return history_.contains(k); }
  std::size_t size() const { return history_.size(); }

  // Time of the most recent access (0 before any access).
  std::uint64_t now() const { return accesses_ == 0 ? 0 : accesses_ - 1; }

  // Writes k's features as of now() into out (out.size() must be kNumFeatures). Missing gaps
  // are NaN. Throws std::out_of_range if k has no history.
  void write_features(Key k, std::span<float> out) const;

  // Column names, in row order. Also the names the trained model must have (checked at load).
  static std::span<const std::string_view, kNumFeatures> feature_names();

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

  std::uint64_t accesses_ = 0;
  std::unordered_map<Key, KeyHistory> history_;
};

}  // namespace lcache
