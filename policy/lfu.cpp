#include "policy/lfu.h"

#include <cassert>
#include <iterator>

namespace lcache {

void LfuPolicy::on_hit(const Request& r) {
  const auto it = index_.find(r.key);
  assert(it != index_.end() && "on_hit: key not tracked");
  newcomer_.reset();  // a new access has started; the previous insert is no longer "current"

  Entry& e = it->second;
  const auto old_bucket = e.bucket;
  // Buckets are sorted and have distinct frequencies, so freq+1 is either the next bucket or
  // doesn't exist yet (then create it right after the current one).
  BucketIt next = std::next(old_bucket);
  if (next == buckets_.end() || next->freq != old_bucket->freq + 1) {
    next = buckets_.insert(next, Bucket{old_bucket->freq + 1, {}});
  }
  // splice moves the list node itself, so e.pos stays valid; appending at the back keeps
  // "reached this frequency earliest" at the front.
  next->keys.splice(next->keys.end(), old_bucket->keys, e.pos);
  e.bucket = next;
  if (old_bucket->keys.empty()) buckets_.erase(old_bucket);
}

void LfuPolicy::on_insert(const Request& r) {
  assert(!index_.contains(r.key) && "on_insert: key already tracked");
  if (buckets_.empty() || buckets_.front().freq != 1) {
    buckets_.push_front(Bucket{1, {}});
  }
  auto& keys = buckets_.front().keys;
  keys.push_back(r.key);
  index_.emplace(r.key, Entry{buckets_.begin(), std::prev(keys.end())});
  newcomer_ = r.key;
}

Key LfuPolicy::evict() {
  assert(!index_.empty() && "evict: nothing tracked");
  const Bucket& lowest = buckets_.front();
  Key victim = lowest.keys.front();
  if (newcomer_ && victim == *newcomer_ && index_.size() > 1) {
    // Skip the newcomer (m2-design 0.2). It sits at the back of the freq-1 bucket, so it is at
    // the front only if it's alone there; then the next bucket holds the true LFU victim.
    victim = lowest.keys.size() > 1 ? *std::next(lowest.keys.begin())
                                    : std::next(buckets_.begin())->keys.front();
  }
  if (newcomer_ == victim) newcomer_.reset();
  erase(index_.find(victim));
  return victim;
}

void LfuPolicy::on_remove(Key k) {
  const auto it = index_.find(k);
  if (it == index_.end()) return;
  if (newcomer_ == k) newcomer_.reset();
  erase(it);
}

void LfuPolicy::erase(std::unordered_map<Key, Entry>::iterator it) {
  const BucketIt bucket = it->second.bucket;
  bucket->keys.erase(it->second.pos);
  if (bucket->keys.empty()) buckets_.erase(bucket);
  index_.erase(it);
}

}  // namespace lcache
