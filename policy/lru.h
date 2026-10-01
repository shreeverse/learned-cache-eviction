#pragma once

#include <list>
#include <string_view>
#include <unordered_map>

#include "policy/eviction_policy.h"

namespace lcache {

// Least Recently Used: evict() returns the tracked key whose last insert or hit is oldest.
// Object size does not affect the order.
//
// Target complexity: O(1) average for every hook; O(n) memory for n tracked keys.
class LruPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "lru"; }

 private:
  // Recency order: front = most recently used, back = least recently used (next victim).
  std::list<Key> order_;
  // Key -> its node in order_, so a hit or remove can find the node in O(1) instead of scanning.
  // std::list iterators stay valid when other nodes are inserted, erased or spliced.
  std::unordered_map<Key, std::list<Key>::iterator> index_;
};

}  // namespace lcache
