#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "trace/request.h"

namespace lcache::test {

// The true optimal number of hits for unit-size objects, by exhaustive search over every choice
// at every miss: skip the object, insert it into free space, or insert it and evict any cached
// object. Memoised on (position, set of cached keys). Exponential; only for tiny traces with
// keys < 32.
class ExhaustiveOpt {
 public:
  ExhaustiveOpt(std::vector<Key> keys, std::size_t capacity)
      : keys_(std::move(keys)), capacity_(capacity) {}

  std::uint64_t max_hits() { return best(0, 0); }

 private:
  std::uint64_t best(std::size_t i, std::uint32_t cached) {
    if (i == keys_.size()) return 0;
    const auto memo_key = std::make_pair(i, cached);
    if (auto it = memo_.find(memo_key); it != memo_.end()) return it->second;

    const std::uint32_t bit = 1U << keys_[i];
    std::uint64_t result = 0;
    if ((cached & bit) != 0) {
      result = 1 + best(i + 1, cached);
    } else {
      result = best(i + 1, cached);  // don't cache it
      if (static_cast<std::size_t>(__builtin_popcount(cached)) < capacity_) {
        result = std::max(result, best(i + 1, cached | bit));
      } else {
        for (std::uint32_t rest = cached; rest != 0; rest &= rest - 1) {
          const std::uint32_t victim = rest & -rest;
          result = std::max(result, best(i + 1, (cached & ~victim) | bit));
        }
      }
    }
    memo_.emplace(memo_key, result);
    return result;
  }

  std::vector<Key> keys_;
  std::size_t capacity_;
  std::map<std::pair<std::size_t, std::uint32_t>, std::uint64_t> memo_;
};

}  // namespace lcache::test
