#pragma once

#include <cstdint>
#include <list>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "policy/eviction_policy.h"

namespace lcache {

// In-cache LFU: evict the key with the fewest accesses since it was inserted; among equal
// frequencies, the one that reached that frequency first (= least recently used). Frequencies
// are forgotten on eviction. Same semantics as libCacheSim's LFU.
//
// evict() never returns the key from the latest on_insert while any other key is tracked, so the
// policy behaves like the textbook evict-then-insert LFU under insert-then-evict (m2-design 0.2).
//
// O(1) per hook: a list of frequency buckets in ascending order, each a list of keys
// (Shah, Mitra & Matani 2010).
class LfuPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "lfu"; }

 private:
  struct Bucket {
    std::uint64_t freq;
    std::list<Key> keys;  // front = reached this frequency earliest
  };
  using BucketIt = std::list<Bucket>::iterator;
  struct Entry {
    BucketIt bucket;
    std::list<Key>::iterator pos;
  };

  // Unlinks k from its bucket (deleting the bucket if it empties) and from index_.
  void erase(std::unordered_map<Key, Entry>::iterator it);

  std::list<Bucket> buckets_;  // ascending freq, never empty buckets
  std::unordered_map<Key, Entry> index_;
  std::optional<Key> newcomer_;  // key from the latest on_insert, until the next hit/remove
};

}  // namespace lcache
