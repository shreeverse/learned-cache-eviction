// lcache-server: the sharded cache over the Redis protocol (RESP2), for redis-cli and
// redis-benchmark (docs/m5-design.md).
#include <CLI/CLI.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "apps/cachesim/size_parse.h"
#include "ml/model_meta.h"
#include "policy/fallback.h"
#include "policy/policy_factory.h"
#include "server/server.h"
#include "server/store.h"

namespace {

// Totals of the per-shard FallbackPolicy counters (decisions D21), to see in benchmarks how
// often the guard tripped and which policy made the evictions.
void print_guard_summary(const lcache::server::ShardedStore& store) {
  lcache::FallbackPolicy::Stats total;
  std::size_t learned_mode = 0;
  store.visit_policies([&](const lcache::EvictionPolicy& p) {
    const auto& g = dynamic_cast<const lcache::FallbackPolicy&>(p);
    const auto& st = g.stats();
    total.learned_evictions += st.learned_evictions;
    total.fallback_evictions += st.fallback_evictions;
    total.hit_rate_trips += st.hit_rate_trips;
    total.latency_trips += st.latency_trips;
    total.budget_breaches += st.budget_breaches;
    total.retries += st.retries;
    if (g.mode() == lcache::FallbackPolicy::Mode::kLearned) ++learned_mode;
  });
  std::cout << "guard: learned_evictions=" << total.learned_evictions
            << " fallback_evictions=" << total.fallback_evictions
            << " hit_rate_trips=" << total.hit_rate_trips
            << " latency_trips=" << total.latency_trips
            << " budget_breaches=" << total.budget_breaches << " retries=" << total.retries
            << " shards_in_learned_mode=" << learned_mode << "/" << store.shard_count()
            << std::endl;
}

}  // namespace

int main(int argc, char** argv) try {
  CLI::App app{"lcache-server: a sharded cache server speaking the Redis protocol"};
  lcache::server::ServerOptions options;
  std::string capacity_arg = "64MiB";
  std::size_t shards = 64;
  std::string policy = "lru";
  lcache::PolicyConfig config;

  app.add_option("--host", options.address, "Address to listen on")->capture_default_str();
  app.add_option("-p,--port", options.port, "TCP port (0 = any free port)")->capture_default_str();
  app.add_option("-t,--threads", options.threads, "I/O threads, one io_context each")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("-c,--capacity", capacity_arg,
                 "Total cache capacity: bytes or KiB/MiB/GiB/TiB, split evenly over the shards; "
                 "an entry is charged key + value bytes")
      ->capture_default_str();
  app.add_option("-s,--shards", shards,
                 "Shards (a power of two), each with its own lock and policy")
      ->capture_default_str();
  app.add_option("--policy", policy,
                 "Eviction policy: lru, lfu, arc, wtinylfu, random-sampled, learned and guarded "
                 "(need --model)")
      ->capture_default_str();
  app.add_option("--model", config.model_path, "LightGBM model for 'learned' and 'guarded'")
      ->check(CLI::ExistingFile);
  app.add_option("--sample-size", config.sample_size, "Candidates per eviction (sampled policies)")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  double budget_us = 0;
  app.add_option("--budget-us", budget_us,
                 "guarded: time budget per learned eviction in microseconds (0 = none)")
      ->capture_default_str();
  app.add_option("--seed", config.seed, "Seed for sampled policies (shard i uses seed + i)")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);
  config.budget_ns = static_cast<std::uint64_t>(budget_us * 1000.0);
  if (!config.model_path.empty()) {
    // Run the model with the settings it was trained with (decisions D20).
    const auto f = lcache::resolve_model_features(config.model_path, {}, false, false);
    config.history = f.history;
    config.history_ratio = f.history_ratio;
  }

  lcache::server::ShardedStore store(lcache::parse_size(capacity_arg), shards,
                                     lcache::server::make_policy_maker(policy, config));
  lcache::server::Server server(store, options);
  server.start();
  std::cout << "lcache-server listening on " << options.address << ":" << server.port()
            << " (threads=" << options.threads << ", shards=" << shards
            << ", capacity=" << capacity_arg << ", policy=" << policy << ")" << std::endl;

  // Block until SIGINT / SIGTERM, then shut down cleanly.
  asio::io_context signals_io;
  asio::signal_set signals(signals_io, SIGINT, SIGTERM);
  signals.async_wait([](std::error_code, int) {});
  signals_io.run();
  server.stop();

  const lcache::server::StoreStats s = store.stats();
  std::cout << "hits=" << s.hits << " misses=" << s.misses << " sets=" << s.sets
            << " evictions=" << s.evictions << " collisions=" << s.collisions
            << " too_large=" << s.too_large << std::endl;
  if (policy == "guarded") print_guard_summary(store);
  return 0;
} catch (const std::exception& e) {
  std::cerr << "lcache-server: " << e.what() << '\n';
  return 1;
}
