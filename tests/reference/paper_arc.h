#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>

#include "trace/request.h"

namespace lcache::test {

// ARC transcribed from Megiddo & Modha, "ARC: A Self-Tuning, Low Overhead Replacement Cache"
// (FAST '03), Fig. 4: unit-size pages, REPLACE runs *before* the new page is added. O(n) deques;
// the point is to read like the paper, not to be fast. In every list, front = LRU, back = MRU.
// p is a double, as in libCacheSim (the paper leaves int vs real open).
class PaperArc {
 public:
  explicit PaperArc(std::size_t c) : c_(c) {}

  bool access(Key x) {
    // Case I: hit in T1 or T2 -> MRU of T2.
    if (contains(x)) {
      erase(t1_, x);
      erase(t2_, x);
      t2_.push_back(x);
      return true;
    }
    // Case II: ghost hit in B1 -> favour recency.
    if (has(b1_, x)) {
      const double d1 = b1_.size() >= b2_.size() ? 1.0 : ratio(b2_.size(), b1_.size());
      p_ = std::min(p_ + d1, static_cast<double>(c_));
      replace(/*x_in_b2=*/false);
      erase(b1_, x);
      t2_.push_back(x);
      return false;
    }
    // Case III: ghost hit in B2 -> favour frequency.
    if (has(b2_, x)) {
      const double d2 = b2_.size() >= b1_.size() ? 1.0 : ratio(b1_.size(), b2_.size());
      p_ = std::max(p_ - d2, 0.0);
      replace(/*x_in_b2=*/true);
      erase(b2_, x);
      t2_.push_back(x);
      return false;
    }
    // Case IV: complete miss.
    if (t1_.size() + b1_.size() == c_) {  // A: |L1| = c
      if (t1_.size() < c_) {
        b1_.pop_front();
        replace(false);
      } else {
        t1_.pop_front();  // B1 is empty: drop the LRU page of T1 without a ghost
      }
    } else if (total() >= c_) {  // B: |L1| < c
      if (total() == 2 * c_) b2_.pop_front();
      replace(false);
    }
    t1_.push_back(x);
    return false;
  }

  bool contains(Key k) const { return has(t1_, k) || has(t2_, k); }
  double p() const { return p_; }

 private:
  void replace(bool x_in_b2) {
    const auto t1 = static_cast<double>(t1_.size());
    if (!t1_.empty() && ((x_in_b2 && t1 == p_) || t1 > p_)) {
      b1_.push_back(t1_.front());
      t1_.pop_front();
    } else if (!t2_.empty()) {
      b2_.push_back(t2_.front());
      t2_.pop_front();
    } else {
      // Not covered by the paper (T2 empty while |T1| <= p); fall back to T1, as implementations
      // do.
      b1_.push_back(t1_.front());
      t1_.pop_front();
    }
  }

  static double ratio(std::size_t a, std::size_t b) {
    return static_cast<double>(a) / static_cast<double>(b);
  }
  static bool has(const std::deque<Key>& l, Key k) {
    return std::find(l.begin(), l.end(), k) != l.end();
  }
  static void erase(std::deque<Key>& l, Key k) {
    if (auto it = std::find(l.begin(), l.end(), k); it != l.end()) l.erase(it);
  }
  std::size_t total() const { return t1_.size() + t2_.size() + b1_.size() + b2_.size(); }

  std::size_t c_;
  double p_ = 0.0;
  std::deque<Key> t1_, t2_, b1_, b2_;
};

}  // namespace lcache::test
