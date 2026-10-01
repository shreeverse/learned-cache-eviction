#include "ml/feature_tracker.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace lcache {

namespace {

constexpr std::uint32_t kU32Max = std::numeric_limits<std::uint32_t>::max();

std::uint32_t saturate_u32(std::uint64_t v) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, kU32Max));
}

// Weight of something that happened `elapsed` requests ago, for a counter with this half-life.
double decay(std::uint64_t elapsed, double half_life) {
  return std::exp2(-static_cast<double>(elapsed) / half_life);
}

constexpr std::array<std::string_view, FeatureTracker::kNumFeatures> kNames{
    "log_size", "age",     "delta_1", "delta_2", "delta_3", "delta_4", "delta_5", "delta_6",
    "delta_7",  "delta_8", "count",   "edc_1",   "edc_2",   "edc_3",   "edc_4"};

}  // namespace

void FeatureTracker::on_access(Key k, std::uint32_t size) {
  const std::uint64_t t = accesses_++;
  const auto [it, inserted] = history_.try_emplace(k);
  Entry& e = it->second;
  KeyHistory& h = e.h;
  if (e.ghost) {  // back from the memory window: continue its history
    ghosts_.erase(e.ghost_pos);
    e.ghost = false;
  }

  if (inserted) {
    h.last_vtime = t;
    h.size = size;
    h.count = 1;
    h.edc.fill(1.0F);
    return;
  }

  const std::uint64_t gap = t - h.last_vtime;
  h.deltas[h.delta_head] = saturate_u32(gap);
  h.delta_head = static_cast<std::uint8_t>((h.delta_head + 1) % kNumDeltas);
  if (h.delta_len < kNumDeltas) ++h.delta_len;

  if (h.count < kU32Max) ++h.count;
  for (std::size_t i = 0; i < kNumEdcs; ++i) {
    h.edc[i] = static_cast<float>(1.0 + h.edc[i] * decay(gap, kEdcHalfLives[i]));
  }
  h.last_vtime = t;
}

void FeatureTracker::write_features(Key k, std::span<float> out) const {
  assert(out.size() == kNumFeatures);
  const KeyHistory& h = history_.at(k).h;
  const std::uint64_t age = now() - h.last_vtime;

  out[kLogSize] =
      static_cast<float>(std::log2(static_cast<double>(std::max<std::uint32_t>(h.size, 1))));
  out[kAge] = static_cast<float>(age);
  for (std::size_t i = 0; i < kNumDeltas; ++i) {
    if (i < h.delta_len) {
      // i-th most recent gap: step back from the slot after the newest one.
      const std::size_t slot = (h.delta_head + kNumDeltas - 1 - i) % kNumDeltas;
      out[kDelta0 + i] = static_cast<float>(h.deltas[slot]);
    } else {
      out[kDelta0 + i] = std::numeric_limits<float>::quiet_NaN();  // LightGBM: missing value
    }
  }
  out[kCount] = static_cast<float>(h.count);
  for (std::size_t i = 0; i < kNumEdcs; ++i) {
    out[kEdc0 + i] = static_cast<float>(h.edc[i] * decay(age, kEdcHalfLives[i]));
  }
}

void FeatureTracker::forget(Key k) {
  const auto it = history_.find(k);
  if (it == history_.end() || it->second.ghost) return;
  if (history_capacity_ == 0) {
    history_.erase(it);
    return;
  }
  it->second.ghost = true;
  it->second.ghost_pos = ghosts_.insert(ghosts_.end(), k);
  if (ghosts_.size() > history_capacity_) {
    history_.erase(ghosts_.front());
    ghosts_.pop_front();
  }
}

bool FeatureTracker::contains(Key k) const {
  const auto it = history_.find(k);
  return it != history_.end() && !it->second.ghost;
}

bool FeatureTracker::remembers(Key k) const {
  const auto it = history_.find(k);
  return it != history_.end() && it->second.ghost;
}

std::span<const std::string_view, FeatureTracker::kNumFeatures> FeatureTracker::feature_names() {
  return kNames;
}

}  // namespace lcache
