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
                 "random-sampled, learned (needs --model)")
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
  app.add_option("--seed", config.seed, "Seed for sampled policies")->capture_default_str();
  app.add_option("--model", config.model_path, "LightGBM model for the 'learned' policy")
      ->check(CLI::ExistingFile);
  app.add_option("-o,--out", out_path, "Write CSV here instead of stdout");

  CLI11_PARSE(app, argc, argv);

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
