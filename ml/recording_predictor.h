#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <unordered_map>

#include "ml/predictor.h"
#include "trace/request.h"

namespace lcache {

// Writes training data (docs/m3-design.md section 5). It wraps another predictor: on every
// predict() call, i.e. every eviction, it first writes one CSV row per candidate, then lets
// `inner` score them. Plugged into LearnedPolicy with an AgePredictor, this is the collector: a
// sampled-LRU cache whose rows come from the same sampling and feature code as inference.
//
// CSV columns: eviction, now, key, the FeatureTracker features (in feature_names() order),
// next_distance. `eviction` numbers the evictions from 0 and groups one candidate set; one
// request can cause many evictions (a large object), so `now` alone doesn't.
// next_distance is the candidate's true next access minus now, or -1 if it is never requested
// again within the replayed requests. `now` uses the same virtual clock as FeatureTracker: it
// counts observe() calls (decisions D11).
class RecordingPredictor final : public Predictor {
 public:
  // Writes the header immediately. Records the candidates of every `every`-th eviction (the 1st,
  // then the (every+1)-th, ...) to keep files small on long traces. Throws if every is 0.
  // `normalized` must match the policy's FeatureConfig (it names the columns); throws if the
  // inner predictor is a model trained on the other kind.
  RecordingPredictor(std::unique_ptr<Predictor> inner, std::ostream& out, std::uint32_t every = 1,
                     bool normalized = false);

  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;
  void observe(const Request& r) override;
  bool needs_next_access() const override { return true; }  // labels need the future
  std::optional<bool> normalized_features() const override { return normalized_; }

  std::uint64_t evictions() const { return evictions_; }
  std::uint64_t rows_written() const { return rows_; }

 private:
  std::unique_ptr<Predictor> inner_;
  std::ostream& out_;
  std::uint32_t every_;
  bool normalized_;
  std::uint64_t observed_ = 0;
  std::uint64_t evictions_ = 0;
  std::uint64_t rows_ = 0;
  std::unordered_map<Key, std::uint64_t> next_;  // key -> next_vtime of its latest request
};

}  // namespace lcache
