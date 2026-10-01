// lcache-export: replays a trace through the training-data collector (sampled LRU) and writes
// one CSV row per eviction candidate: features and the true distance to its next access.
//
//   lcache-export -t data/x.oracleGeneral.bin -c 256MiB -n 56936 -o build/m3/train.csv
//
// Use -n to export only the training prefix, so labels never look into the evaluation half
// (decisions D10). See docs/m3-design.md section 5.
//
// With --model, that model chooses the victims instead of sampled LRU ("on-policy" data: the
// candidates come from the cache the learned policy itself produces).

#include <CLI/CLI.hpp>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "apps/cachesim/size_parse.h"
#ifdef LC_HAVE_LIGHTGBM
#include "ml/lightgbm_predictor.h"
#endif
#include "ml/predictor.h"
#include "ml/recording_predictor.h"
#include "policy/learned.h"
#include "sim/simulator.h"
#include "trace/reader_factory.h"

namespace {

int run(int argc, char** argv) {
  CLI::App app{"Export LearnedPolicy training data"};

  std::string trace_path;
  std::string format = "auto";
  std::string size_arg;
  bool ignore_size = false;
  std::uint64_t max_requests = 0;
  lcache::LearnedOptions options;
  std::uint32_t every = 1;
  std::string model_path;
  std::string out_path;

  app.add_option("-t,--trace", trace_path, "Trace file")->required()->check(CLI::ExistingFile);
  app.add_option("-f,--format", format, "oracle_general | csv | auto (by extension)")
      ->capture_default_str();
  app.add_option("-c,--cache-size", size_arg, "Cache size (bytes, or objects with --ignore-size)")
      ->required();
  app.add_flag("--ignore-size", ignore_size, "Treat every object as size 1");
  app.add_option("-n,--max-requests", max_requests,
                 "Replay only the first N requests (0 = all); labels see only these")
      ->capture_default_str();
  app.add_option("--sample-size", options.sample_size, "Candidates per eviction")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("--history", options.history,
                 "Evicted keys whose feature history is kept (memory window); a model must be "
                 "run with the value its training data was exported with")
      ->capture_default_str();
  app.add_option("--seed", options.seed, "Seed for candidate sampling")->capture_default_str();
  app.add_option("--every", every, "Record the candidates of every N-th eviction")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_option("--model", model_path,
                 "LightGBM model that chooses victims (default: sampled LRU)")
      ->check(CLI::ExistingFile);
  app.add_option("-o,--out", out_path, "Output CSV")->required();

  CLI11_PARSE(app, argc, argv);

  const std::uint64_t capacity = lcache::parse_size(size_arg);
  std::ofstream out(out_path);
  if (!out) throw std::runtime_error("cannot open output file: " + out_path);

  std::unique_ptr<lcache::Predictor> chooser;
  if (model_path.empty()) {
    chooser = std::make_unique<lcache::AgePredictor>();
  } else {
#ifdef LC_HAVE_LIGHTGBM
    chooser = std::make_unique<lcache::LightGbmPredictor>(model_path);
#else
    throw std::runtime_error("--model needs a build with LC_WITH_LIGHTGBM=ON");
#endif
  }
  auto recorder = std::make_unique<lcache::RecordingPredictor>(std::move(chooser), out, every);
  const lcache::RecordingPredictor& stats = *recorder;
  lcache::LearnedPolicy collector(std::move(recorder), "collector", options);

  const auto reader = lcache::make_reader(format, trace_path);
  const auto r = lcache::simulate(
      *reader, collector,
      {.capacity_bytes = capacity, .ignore_size = ignore_size, .max_requests = max_requests});

  out.close();
  if (!out) throw std::runtime_error("error writing " + out_path);
  std::cerr << "requests " << r.requests << ", hit rate " << r.hit_rate() << ", evictions "
            << stats.evictions() << ", rows " << stats.rows_written() << " -> " << out_path << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "lcache-export: " << e.what() << '\n';
    return 1;
  }
}
