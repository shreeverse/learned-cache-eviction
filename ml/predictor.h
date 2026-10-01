#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>

#include "trace/request.h"

namespace lcache {

// Scores eviction candidates for LearnedPolicy (docs/m3-design.md section 2, decisions D14).
// Higher score = next access predicted further in the future = better victim.
class Predictor {
 public:
  Predictor() = default;
  Predictor(const Predictor&) = delete;
  Predictor& operator=(const Predictor&) = delete;
  Predictor(Predictor&&) = delete;
  Predictor& operator=(Predictor&&) = delete;
  virtual ~Predictor() = default;

  // `features` holds keys.size() rows of FeatureTracker::kNumFeatures floats, row-major, in the
  // same order as `keys`. Writes one score per row into `scores` (same size as keys).
  virtual void predict(std::span<const Key> keys, std::span<const float> features,
                       std::span<double> scores) = 0;

  // Called for every request the policy sees (hit or insert). Models ignore it.
  virtual void observe(const Request& /*r*/) {}

  // True if observe() reads Request::next_vtime, i.e. the predictor knows the future.
  virtual bool needs_next_access() const { return false; }
};

// Perfect predictions: the score is the key's true next access, and keys never requested again
// score +infinity. LearnedPolicy with this predictor is sampled Belady, the best any model can do
// with the same candidate sampling. Remembers every key it has observed.
class OraclePredictor final : public Predictor {
 public:
  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;
  void observe(const Request& r) override { next_[r.key] = r.next_vtime; }
  bool needs_next_access() const override { return true; }

 private:
  std::unordered_map<Key, std::uint64_t> next_;
};

// Uniform random scores in [0, 1): evicts a random candidate. A lower bound.
class RandomPredictor final : public Predictor {
 public:
  explicit RandomPredictor(std::uint64_t seed) : rng_(seed) {}
  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;

 private:
  std::mt19937_64 rng_;
};

// Scores each row with a function of (key, feature row). For tests and deliberately bad models.
class FunctionPredictor final : public Predictor {
 public:
  using ScoreFn = std::function<double(Key, std::span<const float>)>;
  explicit FunctionPredictor(ScoreFn fn) : fn_(std::move(fn)) {}

  void predict(std::span<const Key> keys, std::span<const float> features,
               std::span<double> scores) override;

  // For tests: how many predict() calls, and the size of the latest batch.
  std::size_t calls() const { return calls_; }
  std::size_t last_batch_size() const { return last_batch_size_; }

 private:
  ScoreFn fn_;
  std::size_t calls_ = 0;
  std::size_t last_batch_size_ = 0;
};

}  // namespace lcache
