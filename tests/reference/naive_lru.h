#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "trace/request.h"

namespace lcache::test {

// Deliberately simple O(n) LRU over unit-size objects, used as ground truth in differential tests.
// Obviously correct is the goal here, not fast.
class NaiveLru {
 public:
  struct Outcome {
    bool hit = false;
    std::optional<Key> evicted;
  };

  explicit NaiveLru(std::size_t capacity) : capacity_(capacity) {}

  Outcome access(Key k) {
    Outcome out;
    const auto it = std::find(order_.begin(), order_.end(), k);
    if (it != order_.end()) {
      out.hit = true;
      order_.erase(it);
    }
    order_.push_back(k);  // back = most recently used
    if (order_.size() > capacity_) {
      out.evicted = order_.front();
      order_.erase(order_.begin());
    }
    return out;
  }

  void remove(Key k) { std::erase(order_, k); }

  bool contains(Key k) const { return std::find(order_.begin(), order_.end(), k) != order_.end(); }

 private:
  std::size_t capacity_;
  std::vector<Key> order_;  // front = least recently used
};

}  // namespace lcache::test
