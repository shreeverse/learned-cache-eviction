#pragma once

#include <string_view>

#include "trace/request.h"

namespace lcache {

// Decides which object to evict. The policy only tracks metadata (recency, frequency, ...);
// sim::Cache owns the key->size map and byte accounting and calls these hooks (docs/decisions.md
// D1).
//
// Contract (guaranteed by the caller, so implementations need not re-check):
//   - on_hit(r):    r.key is currently tracked by the policy.
//   - on_insert(r): r.key is NOT currently tracked. After the call it is tracked.
//   - evict():      at least one key is tracked. Removes the chosen key from the policy's
//                   metadata and returns it. May return the key that was just inserted.
//   - on_remove(k): stop tracking k (explicit DEL). A no-op if k is not tracked.
class EvictionPolicy {
 public:
  EvictionPolicy() = default;
  EvictionPolicy(const EvictionPolicy&) = delete;
  EvictionPolicy& operator=(const EvictionPolicy&) = delete;
  EvictionPolicy(EvictionPolicy&&) = delete;
  EvictionPolicy& operator=(EvictionPolicy&&) = delete;
  virtual ~EvictionPolicy() = default;

  virtual void on_hit(const Request& r) = 0;
  virtual void on_insert(const Request& r) = 0;
  virtual Key evict() = 0;
  virtual void on_remove(Key k) = 0;

  virtual std::string_view name() const = 0;
};

}  // namespace lcache
