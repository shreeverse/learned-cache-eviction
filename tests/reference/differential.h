#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "policy/eviction_policy.h"
#include "reference/workloads.h"
#include "sim/cache.h"

namespace lcache::test {

// Runs `keys` (unit-size objects) through a real Cache using `policy`, and through `ref`, a simple
// reference model with `bool access(Key)` and `bool contains(Key) const`. After every request the
// hit/miss result and the full cache contents (over [0, key_space)) must agree; equal contents
// after each step means the same key was evicted.
template <typename Ref>
void expect_matches_reference(EvictionPolicy& policy, Ref& ref, std::size_t capacity,
                              const std::vector<Key>& keys, Key key_space) {
  Cache cache(policy, capacity);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const Key k = keys[i];
    ASSERT_EQ(cache.access(req(k)), ref.access(k))
        << "hit/miss differs at step " << i << ", key " << k;
    for (Key c = 0; c < key_space; ++c) {
      ASSERT_EQ(cache.contains(c), ref.contains(c))
          << "contents differ at step " << i << " (key " << k << "), for key " << c;
    }
  }
}

}  // namespace lcache::test
