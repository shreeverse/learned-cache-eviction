#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "trace/request.h"

namespace lcache::test {

// Textbook LFU over unit-size objects, in the usual evict-then-insert form: on a miss with a full
// cache, evict the key with the smallest (frequency, last access time), then insert with frequency
// 1. Frequencies are forgotten on eviction. O(n) per eviction; obviously correct is the goal.
class NaiveLfu {
 public:
  explicit NaiveLfu(std::size_t capacity) : capacity_(capacity) {}

  bool access(Key k) {
    ++tick_;
    if (auto it = entries_.find(k); it != entries_.end()) {
      ++it->second.freq;
      it->second.last = tick_;
      return true;
    }
    if (entries_.size() == capacity_) {
      auto victim = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const auto& [f, l] = it->second;
        if (f < victim->second.freq || (f == victim->second.freq && l < victim->second.last)) {
          victim = it;
        }
      }
      entries_.erase(victim);
    }
    entries_.emplace(k, Entry{1, tick_});
    return false;
  }

  bool contains(Key k) const { return entries_.contains(k); }

 private:
  struct Entry {
    std::uint64_t freq;
    std::uint64_t last;
  };
  std::size_t capacity_;
  std::uint64_t tick_ = 0;
  std::unordered_map<Key, Entry> entries_;
};

}  // namespace lcache::test
