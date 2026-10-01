#include "sim/cache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "policy/lru.h"
#include "reference/naive_lru.h"

namespace lcache {
namespace {

Request req(Key k, std::uint32_t size = 1) { return Request{.key = k, .size = size}; }

// LRU that logs every hook call, to check exactly how Cache drives a policy.
class RecordingPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& r) override {
    log_.push_back("hit " + std::to_string(r.key));
    lru_.on_hit(r);
  }
  void on_insert(const Request& r) override {
    log_.push_back("insert " + std::to_string(r.key));
    lru_.on_insert(r);
  }
  Key evict() override {
    const Key k = lru_.evict();
    log_.push_back("evict " + std::to_string(k));
    return k;
  }
  void on_remove(Key k) override { lru_.on_remove(k); }
  std::string_view name() const override { return "recording"; }

  const std::vector<std::string>& log() const { return log_; }

 private:
  LruPolicy lru_;
  std::vector<std::string> log_;
};

// Always evicts the most recently inserted key, i.e. it rejects newcomers (like an admission
// filter would). Checks the Cache handles a policy that evicts the key it just inserted.
class EvictNewestPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& /*r*/) override {}
  void on_insert(const Request& r) override { stack_.push_back(r.key); }
  Key evict() override {
    const Key k = stack_.back();
    stack_.pop_back();
    return k;
  }
  void on_remove(Key /*k*/) override {}
  std::string_view name() const override { return "evict-newest"; }

 private:
  std::vector<Key> stack_;
};

// A buggy policy that evicts a key the cache never held.
class BogusPolicy final : public EvictionPolicy {
 public:
  void on_hit(const Request& /*r*/) override {}
  void on_insert(const Request& /*r*/) override {}
  Key evict() override { return 999; }
  void on_remove(Key /*k*/) override {}
  std::string_view name() const override { return "bogus"; }
};

TEST(Cache, StartsEmpty) {
  LruPolicy lru;
  const Cache cache(lru, 100);
  EXPECT_EQ(cache.used_bytes(), 0U);
  EXPECT_EQ(cache.object_count(), 0U);
  EXPECT_EQ(cache.capacity_bytes(), 100U);
}

TEST(Cache, MissThenHit) {
  LruPolicy lru;
  Cache cache(lru, 100);
  EXPECT_FALSE(cache.access(req(1, 10)));
  EXPECT_TRUE(cache.access(req(1, 10)));
  EXPECT_TRUE(cache.contains(1));
  EXPECT_EQ(cache.used_bytes(), 10U);
  EXPECT_EQ(cache.object_count(), 1U);
}

TEST(Cache, CallsPolicyHooksInOrder) {
  RecordingPolicy policy;
  Cache cache(policy, 2);
  cache.access(req(1));  // miss
  cache.access(req(2));  // miss
  cache.access(req(1));  // hit
  cache.access(req(3));  // miss -> over capacity -> evicts 2 (LRU)
  EXPECT_EQ(policy.log(),
            (std::vector<std::string>{"insert 1", "insert 2", "hit 1", "insert 3", "evict 2"}));
}

TEST(Cache, InsertsBeforeEvicting) {
  // decisions.md D2: on_insert comes first, then evict() until the cache fits.
  RecordingPolicy policy;
  Cache cache(policy, 1);
  cache.access(req(1));
  cache.access(req(2));
  EXPECT_EQ(policy.log(), (std::vector<std::string>{"insert 1", "insert 2", "evict 1"}));
}

TEST(Cache, EvictsByBytes) {
  LruPolicy lru;
  Cache cache(lru, 10);
  cache.access(req(1, 4));
  cache.access(req(2, 4));
  cache.access(req(3, 4));  // 12 > 10 -> evict 1
  EXPECT_FALSE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
  EXPECT_TRUE(cache.contains(3));
  EXPECT_EQ(cache.used_bytes(), 8U);
}

TEST(Cache, LargeInsertEvictsSeveralObjects) {
  LruPolicy lru;
  Cache cache(lru, 10);
  cache.access(req(1, 3));
  cache.access(req(2, 3));
  cache.access(req(3, 3));
  cache.access(req(4, 9));  // needs 8 bytes freed beyond the 1 spare -> all three go
  EXPECT_EQ(cache.object_count(), 1U);
  EXPECT_TRUE(cache.contains(4));
  EXPECT_EQ(cache.used_bytes(), 9U);
}

TEST(Cache, ObjectExactlyCapacityIsCached) {
  LruPolicy lru;
  Cache cache(lru, 10);
  EXPECT_FALSE(cache.access(req(1, 10)));
  EXPECT_TRUE(cache.access(req(1, 10)));
  EXPECT_EQ(cache.used_bytes(), 10U);
}

TEST(Cache, OversizeObjectIsNeverCachedAndPolicyNotCalled) {
  RecordingPolicy policy;
  Cache cache(policy, 10);
  cache.access(req(1, 5));
  EXPECT_FALSE(cache.access(req(2, 11)));
  EXPECT_FALSE(cache.access(req(2, 11)));  // still a miss
  EXPECT_FALSE(cache.contains(2));
  EXPECT_TRUE(cache.contains(1));  // existing contents untouched
  EXPECT_EQ(cache.used_bytes(), 5U);
  EXPECT_EQ(policy.log(), (std::vector<std::string>{"insert 1"}));
}

TEST(Cache, ZeroCapacityCachesNothing) {
  LruPolicy lru;
  Cache cache(lru, 0);
  EXPECT_FALSE(cache.access(req(1, 1)));
  EXPECT_FALSE(cache.access(req(1, 1)));
  EXPECT_EQ(cache.object_count(), 0U);
}

TEST(Cache, SizeChangeOnHitIsIgnored) {
  // decisions.md D3: a key keeps the size of its first request.
  LruPolicy lru;
  Cache cache(lru, 100);
  cache.access(req(1, 4));
  EXPECT_TRUE(cache.access(req(1, 50)));
  EXPECT_EQ(cache.used_bytes(), 4U);
}

TEST(Cache, PolicyMayEvictTheNewcomer) {
  EvictNewestPolicy policy;
  Cache cache(policy, 2);
  cache.access(req(1));
  cache.access(req(2));
  EXPECT_FALSE(cache.access(req(3)));  // inserted, then immediately chosen as the victim
  EXPECT_FALSE(cache.contains(3));
  EXPECT_TRUE(cache.contains(1));
  EXPECT_TRUE(cache.contains(2));
  EXPECT_EQ(cache.used_bytes(), 2U);
}

TEST(Cache, ThrowsWhenPolicyEvictsUnknownKey) {
  BogusPolicy policy;
  Cache cache(policy, 1);
  cache.access(req(1));
  EXPECT_THROW(cache.access(req(2)), std::logic_error);
}

TEST(Cache, AccountingInvariantsHoldOnRandomTrace) {
  // After every access: used <= capacity, and used == the sum of the sizes charged for the
  // cached keys. A key is charged the size of the request that inserted it; later sizes are
  // ignored while it stays cached (D3), and a re-insert after eviction charges the new size.
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<Key> pick_key(0, 199);
  std::uniform_int_distribution<std::uint32_t> pick_size(1, 300);
  std::unordered_map<Key, std::uint32_t> charged;  // expected size for each key we inserted

  LruPolicy lru;
  Cache cache(lru, 1000);
  for (int i = 0; i < 20'000; ++i) {
    SCOPED_TRACE(::testing::Message() << "step " << i);
    const Key k = pick_key(rng);
    const std::uint32_t size = pick_size(rng);  // sizes vary between requests for the same key

    const bool was_cached = cache.contains(k);
    ASSERT_EQ(cache.access(req(k, size)), was_cached);
    if (!was_cached && cache.contains(k)) charged[k] = size;

    ASSERT_LE(cache.used_bytes(), cache.capacity_bytes());
    std::uint64_t expected_used = 0;
    std::size_t expected_count = 0;
    for (const auto& [key, sz] : charged) {
      if (cache.contains(key)) {
        expected_used += sz;
        ++expected_count;
      }
    }
    ASSERT_EQ(cache.used_bytes(), expected_used);
    ASSERT_EQ(cache.object_count(), expected_count);
  }
}

TEST(Cache, WithLruMatchesNaiveLruHitsOnUnitSizes) {
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<Key> pick_key(0, 49);
  LruPolicy lru;
  Cache cache(lru, 10);
  test::NaiveLru ref(10);
  for (int i = 0; i < 20'000; ++i) {
    const Key k = pick_key(rng);
    ASSERT_EQ(cache.access(req(k)), ref.access(k).hit) << "step " << i << ", key " << k;
  }
}

}  // namespace
}  // namespace lcache
