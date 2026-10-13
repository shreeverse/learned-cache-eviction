#include "server/store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "policy/lru.h"

namespace lcache::server {
namespace {

// A policy that logs every hook call and evicts in FIFO order, or the newest key first (to make
// the newcomer the victim, decisions D2).
class RecordingPolicy final : public EvictionPolicy {
 public:
  enum class Victim : std::uint8_t { Oldest, Newest };

  RecordingPolicy(std::vector<std::string>& log, Victim victim) : log_(log), victim_(victim) {}

  void on_hit(const Request& r) override { log_.push_back("hit " + std::to_string(r.key)); }
  void on_insert(const Request& r) override {
    log_.push_back("insert " + std::to_string(r.key) + " size " + std::to_string(r.size));
    keys_.push_back(r.key);
  }
  Key evict() override {
    const Key k = victim_ == Victim::Oldest ? keys_.front() : keys_.back();
    std::erase(keys_, k);
    log_.push_back("evict " + std::to_string(k));
    return k;
  }
  void on_remove(Key k) override {
    log_.push_back("remove " + std::to_string(k));
    std::erase(keys_, k);
  }
  std::string_view name() const override { return "recording"; }

 private:
  std::vector<std::string>& log_;
  Victim victim_;
  std::vector<Key> keys_;
};

using Log = std::vector<std::string>;

std::optional<std::string> get(Shard& s, std::string_view key, Key id) {
  std::optional<std::string> out;
  s.get(key, id, [&out](std::string_view v) { out.emplace(v); });
  return out;
}

// A shard of 30 bytes with a FIFO recording policy. Keys "a".."z" (1 byte) + 9-byte values
// charge 10 bytes each, so three fit.
struct ShardFixture : testing::Test {
  Log log;
  Shard shard{std::make_unique<RecordingPolicy>(log, RecordingPolicy::Victim::Oldest), 30};
};

const std::string kNine = "123456789";

// ---- Shard: GET / SET / DEL -----------------------------------------------------------------

using ShardTest = ShardFixture;

TEST_F(ShardTest, GetMissDoesNotInsert) {
  EXPECT_EQ(get(shard, "a", 1), std::nullopt);
  EXPECT_TRUE(log.empty());  // cache-aside: the policy never hears about a miss
  EXPECT_EQ(shard.size(), 0U);
  EXPECT_EQ(shard.stats().misses, 1U);
}

TEST_F(ShardTest, SetThenGet) {
  EXPECT_EQ(shard.set("a", 1, kNine), SetResult::Stored);
  EXPECT_EQ(get(shard, "a", 1), kNine);
  EXPECT_EQ(log, (Log{"insert 1 size 10", "hit 1"}));
  EXPECT_EQ(shard.stats().hits, 1U);
  EXPECT_EQ(shard.stats().sets, 1U);
}

TEST_F(ShardTest, ChargesKeyPlusValue) {
  shard.set("abc", 1, "12345");
  EXPECT_EQ(shard.used_bytes(), 8U);
  shard.set("d", 2, "");
  EXPECT_EQ(shard.used_bytes(), 9U);
  EXPECT_EQ(shard.size(), 2U);
}

TEST_F(ShardTest, OverwriteReplacesValueAndIsAHit) {
  shard.set("a", 1, kNine);
  shard.set("a", 1, "xy");
  EXPECT_EQ(get(shard, "a", 1), "xy");
  EXPECT_EQ(shard.used_bytes(), 3U);
  EXPECT_EQ(shard.size(), 1U);
  EXPECT_EQ(log, (Log{"insert 1 size 10", "hit 1", "hit 1"}));
}

TEST_F(ShardTest, DelRemovesFromStoreAndPolicy) {
  shard.set("a", 1, kNine);
  EXPECT_TRUE(shard.del("a", 1));
  EXPECT_EQ(get(shard, "a", 1), std::nullopt);
  EXPECT_EQ(shard.used_bytes(), 0U);
  EXPECT_EQ(log, (Log{"insert 1 size 10", "remove 1"}));
  // The policy no longer tracks it, so it can be inserted again (on_insert's contract).
  shard.set("a", 1, kNine);
  EXPECT_EQ(log.back(), "insert 1 size 10");
}

TEST_F(ShardTest, DelMissingKeyIsANoOp) {
  EXPECT_FALSE(shard.del("a", 1));
  EXPECT_TRUE(log.empty());
}

// ---- Shard: eviction ----------------------------------------------------------------------

TEST_F(ShardTest, EvictsUntilItFits) {
  shard.set("a", 1, kNine);
  shard.set("b", 2, kNine);
  shard.set("c", 3, kNine);
  EXPECT_EQ(shard.used_bytes(), 30U);
  EXPECT_TRUE(log.back().starts_with("insert 3"));  // exactly full: no eviction

  shard.set("d", 4, kNine + kNine);  // 19 bytes: two victims needed
  EXPECT_EQ(get(shard, "a", 1), std::nullopt);
  EXPECT_EQ(get(shard, "b", 2), std::nullopt);
  EXPECT_EQ(get(shard, "c", 3), kNine);
  EXPECT_EQ(get(shard, "d", 4), kNine + kNine);
  EXPECT_EQ(shard.used_bytes(), 29U);
  EXPECT_EQ(shard.stats().evictions, 2U);
}

TEST_F(ShardTest, GrowingAValueCanEvictOthers) {
  shard.set("a", 1, kNine);
  shard.set("b", 2, kNine);
  shard.set("c", 3, kNine);
  shard.set("c", 3, kNine + "x");  // 31 bytes: FIFO evicts "a"
  EXPECT_EQ(get(shard, "a", 1), std::nullopt);
  EXPECT_EQ(get(shard, "c", 3), kNine + "x");
  EXPECT_EQ(shard.used_bytes(), 21U);
}

TEST(Shard, NewcomerCanBeTheVictim) {
  Log log;
  Shard shard(std::make_unique<RecordingPolicy>(log, RecordingPolicy::Victim::Newest), 20);
  shard.set("a", 1, kNine);
  shard.set("b", 2, kNine);
  EXPECT_EQ(shard.set("c", 3, kNine), SetResult::Stored);  // stored, then evicted at once
  EXPECT_EQ(get(shard, "c", 3), std::nullopt);
  EXPECT_EQ(get(shard, "a", 1), kNine);
  EXPECT_EQ(get(shard, "b", 2), kNine);
  EXPECT_EQ(shard.used_bytes(), 20U);
}

TEST_F(ShardTest, ValueLargerThanTheShardIsRejected) {
  shard.set("a", 1, kNine);
  EXPECT_EQ(shard.set("b", 2, std::string(30, 'x')), SetResult::TooLarge);  // 31 bytes
  EXPECT_EQ(get(shard, "a", 1), kNine);                                     // nothing disturbed
  EXPECT_EQ(shard.used_bytes(), 10U);
  EXPECT_EQ(log, (Log{"insert 1 size 10", "hit 1"}));
  EXPECT_EQ(shard.stats().too_large, 1U);
  EXPECT_EQ(shard.set("b", 2, std::string(29, 'x')), SetResult::Stored);  // exactly 30 fits
}

TEST_F(ShardTest, TooLargeOverwriteKeepsTheOldValue) {
  shard.set("a", 1, kNine);
  EXPECT_EQ(shard.set("a", 1, std::string(30, 'x')), SetResult::TooLarge);
  EXPECT_EQ(get(shard, "a", 1), kNine);
}

TEST(Shard, PolicyEvictingAnUnknownKeyIsALogicError) {
  // Tracks keys like LRU but names a key the shard doesn't hold.
  class Bad final : public EvictionPolicy {
   public:
    void on_hit(const Request& r) override { lru_.on_hit(r); }
    void on_insert(const Request& r) override { lru_.on_insert(r); }
    Key evict() override {
      lru_.evict();
      return 999;
    }
    void on_remove(Key k) override { lru_.on_remove(k); }
    std::string_view name() const override { return "bad"; }

   private:
    LruPolicy lru_;
  };
  Shard shard(std::make_unique<Bad>(), 10);
  shard.set("a", 1, kNine);
  EXPECT_THROW(shard.set("b", 2, kNine), std::logic_error);
}

TEST(Shard, RejectsNullPolicy) { EXPECT_THROW(Shard(nullptr, 10), std::invalid_argument); }

// ---- Shard: hash collisions ---------------------------------------------------------------

TEST_F(ShardTest, CollidingSetReplacesTheOtherKey) {
  shard.set("a", 7, kNine);
  shard.set("bb", 7, "12");  // same id, different key
  EXPECT_EQ(get(shard, "a", 7), std::nullopt);
  EXPECT_EQ(get(shard, "bb", 7), "12");
  EXPECT_EQ(shard.used_bytes(), 4U);
  EXPECT_EQ(shard.size(), 1U);
  EXPECT_EQ(log, (Log{"insert 7 size 10", "remove 7", "insert 7 size 4", "hit 7"}));
  EXPECT_EQ(shard.stats().collisions, 1U);
}

TEST_F(ShardTest, CollidingGetIsAMiss) {
  shard.set("a", 7, kNine);
  EXPECT_EQ(get(shard, "b", 7), std::nullopt);  // never another key's value
  EXPECT_EQ(log, (Log{"insert 7 size 10"}));
}

TEST_F(ShardTest, CollidingDelIsANoOp) {
  shard.set("a", 7, kNine);
  EXPECT_FALSE(shard.del("b", 7));
  EXPECT_EQ(get(shard, "a", 7), kNine);
}

// ---- ShardedStore ---------------------------------------------------------------------------

PolicyMaker lru_maker() {
  return [](std::uint64_t, std::size_t) { return std::make_unique<LruPolicy>(); };
}

TEST(DefaultKeyHash, IsDeterministicAndSpreadsKeys) {
  EXPECT_EQ(default_key_hash("user:42"), default_key_hash(std::string("user:") + "42"));
  EXPECT_NE(default_key_hash(""), default_key_hash(std::string(1, '\0')));

  // Sequential keys: no collisions, and the top 6 bits (64 shards) are used evenly.
  std::unordered_set<Key> seen;
  std::vector<int> per_shard(64);
  constexpr int kKeys = 100'000;
  for (int i = 0; i < kKeys; ++i) {
    const Key h = default_key_hash("key:" + std::to_string(i));
    EXPECT_TRUE(seen.insert(h).second);
    ++per_shard[h >> 58];
  }
  // Expected 1562.5 per shard, standard deviation ~39; 6 sigma is far outside chance.
  for (const int n : per_shard) EXPECT_NEAR(n, kKeys / 64.0, 240);
}

TEST(ShardedStore, SetGetDel) {
  ShardedStore store(1 << 20, 8, lru_maker());
  EXPECT_EQ(store.get("k"), std::nullopt);
  EXPECT_EQ(store.set("k", "v"), SetResult::Stored);
  EXPECT_EQ(store.get("k"), "v");
  EXPECT_TRUE(store.del("k"));
  EXPECT_FALSE(store.del("k"));
  EXPECT_EQ(store.get("k"), std::nullopt);
}

TEST(ShardedStore, SplitsCapacityEvenly) {
  ShardedStore store(1000, 8, lru_maker());
  ASSERT_EQ(store.shard_count(), 8U);
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(store.shard(i).capacity_bytes(), 125U);
}

TEST(ShardedStore, KeysLiveInTheirShard) {
  ShardedStore store(1 << 20, 16, lru_maker());
  std::vector<std::size_t> counts(16);
  for (int i = 0; i < 1000; ++i) {
    const std::string key = "key:" + std::to_string(i);
    store.set(key, "v");
    ++counts[store.shard_of(key)];
  }
  for (std::size_t i = 0; i < 16; ++i) EXPECT_EQ(store.shard(i).size(), counts[i]);
}

TEST(ShardedStore, PolicyMakerGetsShardCapacityAndIndex) {
  std::vector<std::pair<std::uint64_t, std::size_t>> calls;
  ShardedStore store(4096, 4, [&calls](std::uint64_t cap, std::size_t shard) {
    calls.emplace_back(cap, shard);
    return std::make_unique<LruPolicy>();
  });
  EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::size_t>>{
                       {1024, 0}, {1024, 1}, {1024, 2}, {1024, 3}}));
}

TEST(ShardedStore, ReinsertedKeyGetsTheSamePolicyKey) {
  // The point of hashing (decisions D22): after an eviction, a SET of the same key reaches the
  // policy with the same id, so the learned policy's FeatureTracker history still applies.
  Log log;
  ShardedStore store(20, 1, [&log](std::uint64_t, std::size_t) {
    return std::make_unique<RecordingPolicy>(log, RecordingPolicy::Victim::Oldest);
  });
  const std::string id = std::to_string(default_key_hash("a"));
  store.set("a", kNine);
  store.set("b", kNine);
  store.set("c", kNine);  // evicts "a"
  ASSERT_EQ(store.get("a"), std::nullopt);
  store.set("a", kNine);
  EXPECT_EQ(log.front(), "insert " + id + " size 10");
  EXPECT_EQ(std::count(log.begin(), log.end(), "insert " + id + " size 10"), 2);
}

TEST(ShardedStore, ForcedCollisionReplaces) {
  ShardedStore store(1 << 20, 4, lru_maker(), [](std::string_view) -> Key { return 42; });
  store.set("first", "1");
  store.set("second", "2");
  EXPECT_EQ(store.get("first"), std::nullopt);
  EXPECT_EQ(store.get("second"), "2");
  EXPECT_EQ(store.stats().collisions, 1U);
  EXPECT_FALSE(store.del("first"));
  EXPECT_TRUE(store.del("second"));
}

TEST(ShardedStore, StatsSumOverShards) {
  ShardedStore store(1 << 20, 8, lru_maker());
  for (int i = 0; i < 100; ++i) store.set("k" + std::to_string(i), "v");
  for (int i = 0; i < 150; ++i) store.get("k" + std::to_string(i));
  const StoreStats s = store.stats();
  EXPECT_EQ(s.sets, 100U);
  EXPECT_EQ(s.hits, 100U);
  EXPECT_EQ(s.misses, 50U);
}

TEST(ShardedStore, VisitPoliciesSeesEveryShardsPolicy) {
  ShardedStore store(1 << 20, 4, lru_maker());
  int visits = 0;
  store.visit_policies([&visits](const EvictionPolicy& p) {
    EXPECT_EQ(p.name(), "lru");
    ++visits;
  });
  EXPECT_EQ(visits, 4);
}

TEST(ShardedStore, RejectsBadConfig) {
  EXPECT_THROW(ShardedStore(1024, 0, lru_maker()), std::invalid_argument);
  EXPECT_THROW(ShardedStore(1024, 3, lru_maker()), std::invalid_argument);
  EXPECT_THROW(ShardedStore(3, 4, lru_maker()), std::invalid_argument);
  EXPECT_THROW(ShardedStore(1024, 4, lru_maker(), nullptr), std::invalid_argument);
  EXPECT_NO_THROW(ShardedStore(4, 4, lru_maker()));
}

TEST(MakePolicyMaker, RejectsUnknownAndFutureKnowledgePolicies) {
  EXPECT_THROW(make_policy_maker("nope", {}), std::invalid_argument);
  EXPECT_THROW(make_policy_maker("belady", {}), std::invalid_argument);
  EXPECT_THROW(make_policy_maker("oracle-sampled", {}), std::invalid_argument);
  EXPECT_THROW(make_policy_maker("learned", {}), std::invalid_argument);  // no model
}

// Every policy a server can run behaves as a cache: never over capacity, and a GET returns
// either a miss or the last value SET for that key.
class StorePolicyTest : public testing::TestWithParam<std::string> {};

TEST_P(StorePolicyTest, RandomOpsKeepCacheInvariants) {
  constexpr std::uint64_t kCapacity = std::uint64_t{8} * 2000;
  ShardedStore store(kCapacity, 8, make_policy_maker(GetParam(), {}));
  std::map<std::string, std::string> last;  // the last value SET per key (absent after DEL)
  std::mt19937_64 rng(3);
  std::uniform_int_distribution<int> key_dist(0, 499);
  std::uniform_int_distribution<int> op_dist(0, 9);
  std::uniform_int_distribution<std::size_t> len_dist(0, 100);
  for (int i = 0; i < 20'000; ++i) {
    const std::string key = "k" + std::to_string(key_dist(rng));
    const int op = op_dist(rng);
    if (op < 5) {
      const std::optional<std::string> got = store.get(key);
      if (got) {
        ASSERT_TRUE(last.contains(key)) << key;
        ASSERT_EQ(*got, last[key]);
      }
    } else if (op < 9) {
      std::string value(len_dist(rng), static_cast<char>('a' + (i % 26)));
      ASSERT_EQ(store.set(key, value), SetResult::Stored);
      last[key] = std::move(value);
    } else {
      store.del(key);
      last.erase(key);
    }
  }
  for (std::size_t s = 0; s < store.shard_count(); ++s) {
    EXPECT_LE(store.shard(s).used_bytes(), store.shard(s).capacity_bytes());
  }
  const StoreStats stats = store.stats();
  EXPECT_GT(stats.hits, 0U);
  EXPECT_GT(stats.evictions, 0U);
}

std::vector<std::string> server_policies() {
  std::vector<std::string> names;
  for (const std::string& n : policy_names()) {
    if (n != "belady" && n != "oracle-sampled") names.push_back(n);
  }
  return names;
}

// Test names may not contain '-'.
std::string test_name(const testing::TestParamInfo<std::string>& p) {
  std::string n = p.param;
  std::replace(n.begin(), n.end(), '-', '_');
  return n;
}

INSTANTIATE_TEST_SUITE_P(Policies, StorePolicyTest, testing::ValuesIn(server_policies()),
                         test_name);

}  // namespace
}  // namespace lcache::server
