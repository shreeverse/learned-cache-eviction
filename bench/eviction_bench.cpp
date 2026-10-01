// Per-eviction latency (M3 step 8, docs/m3-results.md). Run the release build:
//
//   build/release/bench/eviction_bench
//
// Every policy is first warmed by replaying the first 300k requests of the Twitter trace
// (tools/fetch_traces.sh) into a 1 MiB cache, so caches are full and feature histories are
// realistic. Then:
//   evict/<policy>/k:<k>              one evict() plus re-inserting the victim (cache stays full)
//   predict/<model>_<predictor>/k:<k> one Predictor::predict() call on k real feature rows
// <predictor> is lightgbm (the library) or compiled (TreeEnsemblePredictor; identical scores).
// The lru and random-sampled rows show what the re-insert and sampling cost without a model.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "ml/feature_tracker.h"
#include "ml/lightgbm_predictor.h"
#include "ml/tree_ensemble.h"
#include "policy/policy_factory.h"
#include "sim/simulator.h"
#include "trace/reader_factory.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

constexpr std::uint64_t kCapacity = 1 << 20;  // 1 MiB, what the models were trained at
constexpr std::uint64_t kWarmupRequests = 300'000;
constexpr std::size_t kHistory = 10'000;  // the models' memory window (models/README.md)
constexpr std::size_t kF = FeatureTracker::kNumFeatures;

const std::string kTrace = LC_SOURCE_DIR "/data/twitter_cluster26_10M.oracleGeneral.bin";
const std::string kModelSmall = LC_SOURCE_DIR "/models/twitter26_1MiB_h10k_32x31.txt";
const std::string kModelBig = LC_SOURCE_DIR "/models/twitter26_1MiB_h10k_128x63.txt";

const std::vector<Request>& warmup_requests() {
  static const std::vector<Request> reqs = [] {
    const auto reader = make_reader("auto", kTrace);
    std::vector<Request> out;
    out.reserve(kWarmupRequests);
    Request r;
    while (out.size() < kWarmupRequests && reader->next(r)) out.push_back(r);
    return out;
  }();
  return reqs;
}

// 4,096 feature rows of keys requested near the end of the warmup, i.e. keys a cache would hold.
const std::vector<float>& realistic_rows() {
  static const std::vector<float> rows = [] {
    const auto& reqs = warmup_requests();
    FeatureTracker ft;
    for (const Request& r : reqs) ft.on_access(r.key, r.size);
    std::mt19937_64 rng(1);
    std::vector<float> out(4096 * kF);
    for (std::size_t i = 0; i < 4096; ++i) {
      const Request& r = reqs[reqs.size() - 1 - rng() % 50'000];
      ft.write_features(r.key, std::span(out).subspan(i * kF, kF));
    }
    return out;
  }();
  return rows;
}

// A warmed policy per configuration, built on first use. Google Benchmark calls each benchmark
// function several times (to size the iteration count, and per repetition); warming 1M requests
// through a model every time would take minutes. Later calls continue from the previous state,
// which stays a full cache because every eviction is followed by a re-insert.
EvictionPolicy& warmed_policy(const std::string& policy_name, const std::string& model,
                              const std::string& predictor, std::uint32_t k) {
  static std::map<std::string, std::unique_ptr<EvictionPolicy>> cache;
  auto& slot = cache[policy_name + "|" + model + "|" + predictor + "|" + std::to_string(k)];
  if (!slot) {
    const PolicyConfig config{.capacity_bytes = kCapacity,
                              .model_path = model,
                              .predictor = predictor,
                              .sample_size = k,
                              .seed = 1,
                              .history = kHistory};
    slot = make_policy(policy_name, config);
    VectorReader reader(warmup_requests());
    simulate(reader, *slot, {.capacity_bytes = kCapacity});
  }
  return *slot;
}

void evict_cycle(benchmark::State& state, const std::string& policy_name, const std::string& model,
                 const std::string& predictor, std::uint32_t k) {
  EvictionPolicy& policy = warmed_policy(policy_name, model, predictor, k);
  for (auto _ : state) {
    Key victim = policy.evict();
    policy.on_insert(Request{.key = victim, .size = 1});
    benchmark::DoNotOptimize(victim);
  }
}

template <typename P>
void predict_batch(benchmark::State& state, const std::string& model, std::size_t k) {
  P predictor(model);
  const auto& rows = realistic_rows();
  const std::vector<Key> keys(k, 0);
  std::vector<double> scores(k);
  std::size_t offset = 0;
  for (auto _ : state) {
    predictor.predict(keys, std::span(rows).subspan(offset * kF, k * kF), scores);
    benchmark::DoNotOptimize(scores.data());
    offset = (offset + k) % (4096 - k);  // vary the rows between iterations
  }
}

void register_all() {
  const std::vector<std::uint32_t> ks{16, 32, 64};
  const std::vector<std::pair<std::string, std::string>> models{{"32x31", kModelSmall},
                                                                {"128x63", kModelBig}};
  benchmark::RegisterBenchmark("evict/lru", evict_cycle, "lru", "", "", 64);
  benchmark::RegisterBenchmark("evict/wtinylfu", evict_cycle, "wtinylfu", "", "", 64);
  for (const std::uint32_t k : ks) {
    const std::string kk = "/k:" + std::to_string(k);
    benchmark::RegisterBenchmark("evict/random-sampled" + kk, evict_cycle, "random-sampled", "", "",
                                 k);
    for (const auto& [label, path] : models) {
      for (const char* predictor : {"lightgbm", "compiled"}) {
        const std::string name = label + "_" + predictor + kk;
        benchmark::RegisterBenchmark("evict/learned_" + name, evict_cycle, "learned", path,
                                     predictor, k);
      }
      benchmark::RegisterBenchmark("predict/" + label + "_lightgbm" + kk,
                                   predict_batch<LightGbmPredictor>, path, k);
      benchmark::RegisterBenchmark("predict/" + label + "_compiled" + kk,
                                   predict_batch<TreeEnsemblePredictor>, path, k);
    }
  }
}

}  // namespace
}  // namespace lcache

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  lcache::register_all();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
