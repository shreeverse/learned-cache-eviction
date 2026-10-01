// cachesim: replays a trace against one or more policies and cache sizes, printing CSV results.
//
//   cachesim --trace data/x.oracleGeneral.bin --policy lru --cache-size 64MiB,256MiB,1GiB
//
// One row per (policy, cache size). Each run uses a fresh policy and rewinds the trace.

#include <CLI/CLI.hpp>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "apps/cachesim/size_parse.h"
#include "ml/model_meta.h"
#include "policy/fallback.h"
#include "policy/policy_factory.h"
#include "sim/simulator.h"
#include "trace/reader_factory.h"

namespace {

int run(int argc, char** argv) {
  CLI::App app{"Trace-driven cache simulator"};

  std::string trace_path;
  std::string format = "auto";
  std::vector<std::string> policies{"lru"};
  std::vector<std::string> size_args;
  bool ignore_size = false;
  std::uint64_t max_requests = 0;
  std::uint64_t warmup = 0;
  lcache::PolicyConfig config;
  std::string out_path;

  app.add_option("-t,--trace", trace_path, "Trace file")->required()->check(CLI::ExistingFile);
  app.add_option("-f,--format", format, "oracle_general | csv | auto (by extension)")
      ->capture_default_str();
  app.add_option("-p,--policy", policies,
                 "Policies, comma-separated: lru, lfu, arc, wtinylfu, belady, oracle-sampled, "
                 "random-sampled, learned and guarded (need --model)")
      ->delimiter(',')
      ->capture_default_str();
  app.add_option("-c,--cache-size", size_args,
                 "Cache sizes, comma-separated: bytes (1048576) or KiB/MiB/GiB/TiB; "
                 "with --ignore-size, a number of objects")
      ->delimiter(',')
      ->required();
  app.add_flag("--ignore-size", ignore_size, "Treat every object as size 1");
  app.add_option("-n,--max-requests", max_requests, "Stop after N requests (0 = all)")
      ->capture_default_str();
  app.add_option("-w,--warmup", warmup,
                 "Replay the first N requests but leave them out of the results (counts toward -n)")
      ->capture_default_str();
  app.add_option("--sample-size", config.sample_size,
                 "Candidates per eviction for sampled policies (oracle-sampled, random-sampled)")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("--history", config.history,
                 "Sampled policies: evicted keys whose feature history is kept (fixed window). "
                 "For 'learned', taken from <model>.meta when that exists")
      ->capture_default_str();
  app.add_option("--history-ratio", config.history_ratio,
                 "Memory window as a multiple of the cached object count (overrides --history)")
      ->capture_default_str();
  app.add_option("--guard-window", config.guard_window,
                 "guarded: requests in the rolling hit-rate comparison with shadow W-TinyLFU")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("--guard-margin", config.guard_margin,
                 "guarded: fall back when behind W-TinyLFU by more than this hit-rate fraction")
      ->capture_default_str();
  app.add_option("--guard-grace", config.guard_grace,
                 "guarded: requests after each switch to learned before it is judged")
      ->capture_default_str();
  app.add_flag("!--no-early-check", config.guard_early_check,
               "guarded: judge only full windows (no early 3*sqrt(n) check)");
  app.add_option("--guard-cooldown", config.guard_cooldown,
                 "guarded: requests on W-TinyLFU before retrying the learned policy")
      ->capture_default_str();
  double budget_us = 0;
  app.add_option("--budget-us", budget_us,
                 "guarded: time budget per learned eviction in microseconds (0 = none); "
                 "makes results depend on machine speed")
      ->capture_default_str();
  app.add_option("--max-breaches", config.max_breaches,
                 "guarded: evictions over budget that trip the fallback (1 = at once)")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("--seed", config.seed, "Seed for sampled policies")->capture_default_str();
  app.add_option("--model", config.model_path, "LightGBM model for the 'learned' policy")
      ->check(CLI::ExistingFile);
  app.add_option("--predictor", config.predictor,
                 "How 'learned' evaluates the model: compiled (fast, regression models) | "
                 "lightgbm (the LightGBM library, any model)")
      ->check(CLI::IsMember({"compiled", "lightgbm"}))
      ->capture_default_str();
  app.add_option("-o,--out", out_path, "Write CSV here instead of stdout");

  CLI11_PARSE(app, argc, argv);
  config.budget_ns = static_cast<std::uint64_t>(budget_us * 1000.0);

  if (!config.model_path.empty()) {
    // Run the model with the settings it was trained with (decisions D20).
    const auto f = lcache::resolve_model_features(
        config.model_path, {.history = config.history, .history_ratio = config.history_ratio},
        app.count("--history") > 0, app.count("--history-ratio") > 0);
    config.history = f.history;
    config.history_ratio = f.history_ratio;
  }

  std::vector<std::uint64_t> sizes;
  sizes.reserve(size_args.size());
  for (const auto& s : size_args) sizes.push_back(lcache::parse_size(s));
  // Validate names and options before any long run.
  for (const auto& p : policies) {
    lcache::PolicyConfig probe = config;
    probe.capacity_bytes = 1;
    lcache::make_policy(p, probe);
  }

  std::ofstream file;
  if (!out_path.empty()) {
    file.open(out_path);
    if (!file) throw std::runtime_error("cannot open output file: " + out_path);
  }
  std::ostream& out = out_path.empty() ? std::cout : file;

  const auto reader = lcache::make_reader(format, trace_path);

  out << "trace,policy,cache_size,ignore_size,requests,hits,hit_rate,bytes_requested,bytes_hit,"
         "byte_hit_rate,warmup_requests\n";
  out << std::setprecision(10);
  for (const auto& policy_name : policies) {
    for (const std::uint64_t size : sizes) {
      config.capacity_bytes = size;
      const auto policy = lcache::make_policy(policy_name, config);
      const auto r = lcache::simulate(*reader, *policy,
                                      {.capacity_bytes = size,
                                       .ignore_size = ignore_size,
                                       .max_requests = max_requests,
                                       .warmup_requests = warmup});
      out << trace_path << ',' << policy->name() << ',' << size << ','
          << (ignore_size ? "true" : "false") << ',' << r.requests << ',' << r.hits << ','
          << r.hit_rate() << ',' << r.bytes_requested << ',' << r.bytes_hit << ','
          << r.byte_hit_rate() << ',' << warmup << '\n';
      if (const auto* g = dynamic_cast<const lcache::FallbackPolicy*>(policy.get())) {
        const auto& st = g->stats();
        std::cerr << "guarded @" << size << ": evictions learned " << st.learned_evictions
                  << ", W-TinyLFU " << st.fallback_evictions << "; hit-rate trips "
                  << st.hit_rate_trips << ", latency trips " << st.latency_trips
                  << " (budget breaches " << st.budget_breaches << "), retries " << st.retries
                  << '\n';
      }
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "cachesim: " << e.what() << '\n';
    return 1;
  }
}
