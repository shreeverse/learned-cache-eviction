#pragma once

#include <string_view>

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
  // TODO(shree): data members.
};

}  // namespace lcache
