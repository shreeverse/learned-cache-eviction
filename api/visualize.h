#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "policy/eviction_policy.h"

namespace lcache::api {

// Pieces of POST /api/visualize-window (decisions D27): see what a policy does on each request
// without changing sim::Cache or the policies.

// Forwards every call to the wrapped policy and records the insert and the victims of each
// Cache::access, so the caller can see which keys left the cache. Call clear() before each
// access.
class RecordingPolicy final : public EvictionPolicy {
 public:
  explicit RecordingPolicy(std::unique_ptr<EvictionPolicy> inner) : inner_(std::move(inner)) {}

  void on_hit(const Request& r) override { inner_->on_hit(r); }
  void on_insert(const Request& r) override {
    inserted_ = r.key;
    inner_->on_insert(r);
  }
  Key evict() override {
    const Key victim = inner_->evict();
    evicted_.push_back(victim);
    return victim;
  }
  void on_remove(Key k) override { inner_->on_remove(k); }
  std::string_view name() const override { return inner_->name(); }
  bool needs_next_access() const override { return inner_->needs_next_access(); }

  // The key inserted since the last clear(), if any (a miss that fit in the cache).
  std::optional<Key> inserted() const { return inserted_; }
  // Victims since the last clear(), in eviction order. May contain the inserted key (D2).
  const std::vector<Key>& evicted() const { return evicted_; }
  void clear() {
    inserted_.reset();
    evicted_.clear();
  }

 private:
  std::unique_ptr<EvictionPolicy> inner_;
  std::optional<Key> inserted_;
  std::vector<Key> evicted_;
};

// Gives each cached key a fixed position for drawing: a removed key frees its slot and an added
// key takes the lowest free slot, so keys that stay cached never move.
class SlotMap {
 public:
  explicit SlotMap(std::size_t slots) : slots_(slots) {}

  // Applies one access recorded by `rec`: victims leave first, then the inserted key (unless it
  // was itself evicted) takes a slot. Throws std::logic_error if the events are inconsistent
  // with the map (a full map, or a victim that has no slot).
  void apply(const RecordingPolicy& rec);

  // The slot holding `k`, if any.
  std::optional<std::size_t> find(Key k) const;
  const std::vector<std::optional<Key>>& slots() const { return slots_; }

 private:
  void add(Key k);
  void remove(Key k);

  std::vector<std::optional<Key>> slots_;
};

// Short display labels: 0 -> "A", 25 -> "Z", 26 -> "AA", 27 -> "AB", ... (bijective base 26).
std::string key_label(std::size_t index);

}  // namespace lcache::api
