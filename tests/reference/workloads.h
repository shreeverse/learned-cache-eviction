#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "trace/request.h"

namespace lcache::test {

inline Request req(Key k, std::uint32_t size = 1) { return Request{.key = k, .size = size}; }

// n keys in [0, key_space), Zipf(alpha)-distributed (key 0 is the most popular). Deterministic.
inline std::vector<Key> zipf_keys(std::size_t n, Key key_space, double alpha, std::uint64_t seed) {
  std::vector<double> weights(key_space);
  for (Key k = 0; k < key_space; ++k)
    weights[k] = 1.0 / std::pow(static_cast<double>(k + 1), alpha);
  std::discrete_distribution<Key> dist(weights.begin(), weights.end());
  std::mt19937_64 rng(seed);
  std::vector<Key> keys(n);
  for (auto& k : keys) k = dist(rng);
  return keys;
}

// n keys uniform in [0, key_space). Deterministic.
inline std::vector<Key> uniform_keys(std::size_t n, Key key_space, std::uint64_t seed) {
  std::uniform_int_distribution<Key> dist(0, key_space - 1);
  std::mt19937_64 rng(seed);
  std::vector<Key> keys(n);
  for (auto& k : keys) k = dist(rng);
  return keys;
}

}  // namespace lcache::test
