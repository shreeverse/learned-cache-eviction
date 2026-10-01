#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "policy/eviction_policy.h"

namespace lcache {

// ARC (Megiddo & Modha, FAST '03), generalised to bytes like libCacheSim: list sizes, the target p
// and the capacity c are in bytes (p is a double). Resident lists T1 (seen once recently) and T2
// (seen at least twice); ghost lists B1/B2 remember recently evicted keys and steer p.
//
// Mapping onto insert-then-evict (docs/m2-design.md section 2): on_insert does the paper's cases
// II-IV minus REPLACE; evict() is REPLACE with the newcomer excluded; once the resident lists fit,
// ghost lists are trimmed so |T1|+|B1| <= c and |T1|+|T2|+|B1|+|B2| <= 2c. For unit sizes this is
// step-for-step identical to the paper (checked against tests/reference/paper_arc.h).
//
// Ghost keys are not "tracked" in the EvictionPolicy sense: on_insert may be called for them.
// O(1) average per hook (ghost trimming amortised O(1)).
class ArcPolicy final : public EvictionPolicy {
 public:
  explicit ArcPolicy(std::uint64_t capacity_bytes) : c_(capacity_bytes) {}

  void on_hit(const Request& r) override;
  void on_insert(const Request& r) override;
  Key evict() override;
  void on_remove(Key k) override;

  std::string_view name() const override { return "arc"; }

  // Sizes in bytes, for tests and debugging.
  struct State {
    double p;
    std::uint64_t t1, t2, b1, b2;
  };
  State state() const;

 private:
  enum ListId : std::uint8_t { kT1 = 0, kT2 = 1, kB1 = 2, kB2 = 3 };
  struct Entry {
    ListId list;
    std::list<Key>::iterator pos;
    std::uint32_t size;
  };
  using Index = std::unordered_map<Key, Entry>;

  // Moves an entry to the MRU end of `to`, keeping byte counts in sync.
  void move_to(Entry& e, ListId to);
  // Removes a key (which must be indexed) from its list and from the index.
  void drop(Key k);
  // Once the resident lists fit in c, drops LRU ghosts until |T1|+|B1| <= c and the whole
  // directory is <= 2c.
  void trim_ghosts();

  std::uint64_t c_;
  double p_ = 0.0;
  std::array<std::list<Key>, 4> lists_;  // front = LRU, back = MRU
  std::array<std::uint64_t, 4> bytes_{};
  Index index_;
  std::optional<Key> newcomer_;    // key of the latest on_insert, until the next hit/remove
  bool newcomer_from_b2_ = false;  // REPLACE's tie rule ("x in B2 and |T1| = p")
};

}  // namespace lcache
