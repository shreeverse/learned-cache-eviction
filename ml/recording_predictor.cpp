#include "ml/recording_predictor.h"

#include <array>
#include <cassert>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ml/feature_tracker.h"

namespace lcache {

namespace {

// Shortest text that reads back as the same value (NaN is written as "nan"), and much faster
// than ostream formatting, which matters for millions of rows.
template <typename T>
void put(std::ostream& out, T value) {
  std::array<char, 32> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  assert(ec == std::errc());
  out.write(buf.data(), end - buf.data());
}

}  // namespace

RecordingPredictor::RecordingPredictor(std::unique_ptr<Predictor> inner, std::ostream& out,
                                       std::uint32_t every)
    : inner_(std::move(inner)), out_(out), every_(every) {
  if (!inner_) throw std::invalid_argument("RecordingPredictor: inner predictor is null");
  if (every_ == 0) throw std::invalid_argument("RecordingPredictor: every is 0");
  out_ << "eviction,now,key";
  for (const std::string_view name : FeatureTracker::feature_names()) out_ << ',' << name;
  out_ << ",next_distance\n";
}

void RecordingPredictor::observe(const Request& r) {
  ++observed_;
  next_[r.key] = r.next_vtime;
  inner_->observe(r);
}

void RecordingPredictor::predict(std::span<const Key> keys, std::span<const float> features,
                                 std::span<double> scores) {
  constexpr std::size_t kF = FeatureTracker::kNumFeatures;
  const std::uint64_t eviction = evictions_++;
  if (eviction % every_ == 0) {
    const std::uint64_t now = observed_ - 1;  // predict() always follows at least one observe()
    for (std::size_t i = 0; i < keys.size(); ++i) {
      put(out_, eviction);
      out_.put(',');
      put(out_, now);
      out_.put(',');
      put(out_, keys[i]);
      for (std::size_t j = 0; j < kF; ++j) {
        out_.put(',');
        put(out_, features[i * kF + j]);
      }
      out_.put(',');
      // The candidate hasn't been requested since its latest request, so that request's
      // next_vtime is exactly its next access.
      const std::uint64_t next = next_.at(keys[i]);
      if (next == kNever) {
        out_ << "-1";
      } else {
        put(out_, next - now);
      }
      out_.put('\n');
    }
    rows_ += keys.size();
  }
  inner_->predict(keys, features, scores);
}

}  // namespace lcache
