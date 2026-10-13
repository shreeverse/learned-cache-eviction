// Concurrency stress test for ShardedStore: many threads, one store, constant eviction. Meant to
// run under TSan (decisions D5, D22) as well as in normal builds.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ml/model_meta.h"
#include "policy/policy_factory.h"
#include "server/store.h"

namespace lcache::server {
namespace {

constexpr std::size_t kThreads = 8;
constexpr int kOpsPerThread = 20'000;
constexpr int kKeys = 2'000;
constexpr std::size_t kShards = 16;
// ~8 KiB per shard holds ~60 average entries of the ~2,000 keys: almost every SET evicts.
constexpr std::uint64_t kCapacity = kShards * 8 * 1024;

// Values identify their key and writer and carry padding derived from the sequence number, so a
// value read for the wrong key, or one torn by a race, fails check_value().
std::string make_value(std::string_view key, std::size_t thread, std::uint64_t seq,
                       std::size_t pad_len) {
  std::string v(key);
  v += '#' + std::to_string(thread) + '#' + std::to_string(seq) + '#';
  v.append(pad_len, static_cast<char>('a' + seq % 26));
  return v;
}

bool check_value(std::string_view key, std::string_view v) {
  if (!v.starts_with(key) || v.size() <= key.size() || v[key.size()] != '#') return false;
  v.remove_prefix(key.size() + 1);
  const std::size_t thread_end = v.find('#');
  if (thread_end == std::string_view::npos) return false;
  v.remove_prefix(thread_end + 1);
  const std::size_t seq_end = v.find('#');
  if (seq_end == std::string_view::npos) return false;
  const std::uint64_t seq = std::stoull(std::string(v.substr(0, seq_end)));
  const std::string_view pad = v.substr(seq_end + 1);
  const char expected = static_cast<char>('a' + seq % 26);
  return std::ranges::all_of(pad, [expected](char c) { return c == expected; });
}

TEST(StressHelpers, CheckValueCatchesWrongKeysAndTornValues) {
  const std::string v = make_value("key:1", 3, 28, 10);  // 28 % 26 = 2 -> padding 'c'
  EXPECT_TRUE(check_value("key:1", v));
  EXPECT_FALSE(check_value("key:2", v));
  EXPECT_FALSE(check_value("key:", v));
  std::string torn = v;
  torn.back() = 'd';
  EXPECT_FALSE(check_value("key:1", torn));
}

struct WorkerCounts {
  std::uint64_t gets = 0;
  std::uint64_t hits = 0;
  std::uint64_t sets = 0;
  std::uint64_t bad_values = 0;

  WorkerCounts& operator+=(const WorkerCounts& o) {
    gets += o.gets;
    hits += o.hits;
    sets += o.sets;
    bad_values += o.bad_values;
    return *this;
  }
};

// One worker: 50% GET (checking every value it reads), 40% SET, 10% DEL, over a skewed key
// distribution (squared uniform) so some keys are hot and their shards contended.
WorkerCounts run_worker(ShardedStore& store, std::size_t thread) {
  WorkerCounts c;
  std::mt19937_64 rng(1000 + thread);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::uniform_int_distribution<int> op(0, 9);
  std::uniform_int_distribution<std::size_t> pad(0, 200);
  for (int i = 0; i < kOpsPerThread; ++i) {
    const double x = u(rng);
    const std::string key = "key:" + std::to_string(static_cast<int>(x * x * kKeys));
    const int o = op(rng);
    if (o < 5) {
      ++c.gets;
      const std::optional<std::string> v = store.get(key);
      if (!v) continue;
      ++c.hits;
      if (!check_value(key, *v)) ++c.bad_values;
    } else if (o < 9) {
      ++c.sets;
      store.set(key, make_value(key, thread, static_cast<std::uint64_t>(i), pad(rng)));
    } else {
      store.del(key);
    }
  }
  return c;
}

// Checks used_bytes <= capacity on every shard, over and over, until stopped.
class CapacityChecker {
 public:
  explicit CapacityChecker(const ShardedStore& store) : store_(store), thread_([this] { run(); }) {}
  CapacityChecker(const CapacityChecker&) = delete;
  CapacityChecker& operator=(const CapacityChecker&) = delete;
  CapacityChecker(CapacityChecker&&) = delete;
  CapacityChecker& operator=(CapacityChecker&&) = delete;
  ~CapacityChecker() { stop(); }

  void stop() {
    done_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }
  std::uint64_t violations() const { return violations_.load(); }
  std::uint64_t rounds() const { return rounds_.load(); }

 private:
  void run() {
    while (!done_.load(std::memory_order_acquire)) {
      for (std::size_t s = 0; s < store_.shard_count(); ++s) {
        if (store_.shard(s).used_bytes() > store_.shard(s).capacity_bytes()) ++violations_;
      }
      ++rounds_;
    }
  }

  const ShardedStore& store_;
  std::atomic<bool> done_{false};
  std::atomic<std::uint64_t> violations_{0};
  std::atomic<std::uint64_t> rounds_{0};
  std::thread thread_;  // last: starts after the members it uses are initialized
};

PolicyConfig config_for(const std::string& policy) {
  PolicyConfig config;
  if (policy == "learned" || policy == "guarded") {
    config.model_path = std::string(LC_MODELS_DIR) + "/twitter26_1MiB_h10k_32x31.txt";
    const FeatureConfig f = resolve_model_features(config.model_path, {}, false, false);
    config.history = f.history;
    config.history_ratio = f.history_ratio;
  }
  return config;
}

class StoreStressTest : public testing::TestWithParam<std::string> {};

TEST_P(StoreStressTest, ConcurrentOpsKeepInvariants) {
  ShardedStore store(kCapacity, kShards, make_policy_maker(GetParam(), config_for(GetParam())));

  CapacityChecker checker(store);
  std::vector<WorkerCounts> counts(kThreads);
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t t = 0; t < kThreads; ++t) {
    workers.emplace_back([&store, &counts, t] { counts[t] = run_worker(store, t); });
  }
  for (std::thread& w : workers) w.join();
  checker.stop();

  WorkerCounts total;
  for (const WorkerCounts& c : counts) total += c;
  EXPECT_EQ(total.bad_values, 0U);
  EXPECT_EQ(checker.violations(), 0U);
  EXPECT_GT(checker.rounds(), 0U);
  for (std::size_t s = 0; s < store.shard_count(); ++s) {
    EXPECT_LE(store.shard(s).used_bytes(), store.shard(s).capacity_bytes()) << "shard " << s;
  }
  // The store's counters agree exactly with what the workers did.
  const StoreStats stats = store.stats();
  EXPECT_EQ(stats.hits, total.hits);
  EXPECT_EQ(stats.hits + stats.misses, total.gets);
  EXPECT_EQ(stats.sets, total.sets);
  EXPECT_EQ(stats.too_large, 0U);
  EXPECT_GT(stats.evictions, 0U);  // eviction really ran under contention
  EXPECT_GT(total.hits, 0U);
}

std::string test_name(const testing::TestParamInfo<std::string>& p) {
  std::string n = p.param;
  std::replace(n.begin(), n.end(), '-', '_');
  return n;
}

INSTANTIATE_TEST_SUITE_P(Policies, StoreStressTest,
                         testing::Values("lru", "lfu", "arc", "wtinylfu", "random-sampled",
                                         "learned", "guarded"),
                         test_name);

}  // namespace
}  // namespace lcache::server
