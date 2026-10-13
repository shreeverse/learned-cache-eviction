#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "policy/policy_factory.h"

namespace lcache::api {

// The simulation API's logic, without HTTP (decisions D26): each endpoint maps JSON to JSON and
// reports a client error by throwing ApiError. api/main.cpp only wires these to routes.

// An error with the HTTP status to answer with (400 for invalid input).
class ApiError : public std::runtime_error {
 public:
  ApiError(int status, const std::string& message) : std::runtime_error(message), status_(status) {}
  int status() const { return status_; }

 private:
  int status_;
};

struct TraceInfo {
  std::string name;              // file name; the only way a client refers to a trace
  std::string format;            // "oracle_general" or "csv"
  std::uint64_t bytes = 0;       // file size
  std::uint64_t requests = 0;    // records in the file (oracleGeneral: zero-size ones are
                                 // skipped at replay, so this is an upper bound)
  std::filesystem::path path{};  // server side only, never sent
};

// Trace files directly in `dir` that the readers support (*.oracleGeneral.bin, *.csv), sorted by
// name. Other files (e.g. the compressed .zst download) are skipped. Throws if `dir` is missing.
std::vector<TraceInfo> list_traces(const std::filesystem::path& dir);

struct ApiOptions {
  std::filesystem::path data_dir = "data";
  // Model for "learned" and "guarded"; empty or unloadable makes them unavailable.
  std::string model_path{};
  unsigned workers = 8;  // (policy, cache size) runs simulated in parallel
};

// Limits on one POST /api/simulate, so a request stays bounded in time and memory.
struct Limits {
  static constexpr std::uint64_t kDefaultRequests = 1'000'000;
  static constexpr std::uint64_t kMaxRequests = 10'000'000;
  static constexpr std::size_t kMaxPolicies = 7;
  static constexpr std::size_t kMaxSizes = 12;
  static constexpr std::uint64_t kMinSize = 1ULL << 10;  // 1 KiB
  static constexpr std::uint64_t kMaxSize = 1ULL << 40;  // 1 TiB
  static constexpr std::uint32_t kDefaultPoints = 200;
  static constexpr std::uint32_t kMinPoints = 10;
  static constexpr std::uint32_t kMaxPoints = 1000;
};

// A validated simulate request.
struct SimulateRequest {
  TraceInfo trace;
  std::vector<std::string> policies;       // ids, in the order given, no duplicates
  std::vector<std::uint64_t> cache_sizes;  // bytes, ascending, no duplicates
  std::uint64_t max_requests = Limits::kDefaultRequests;
  std::uint64_t warmup = 0;
  std::uint32_t series_points = Limits::kDefaultPoints;
};

class Api {
 public:
  explicit Api(ApiOptions options);

  // GET /api/traces: [{name, format, bytes, requests}]
  nlohmann::json traces() const;
  // GET /api/policies: [{id, label, offline, available, note}]
  nlohmann::json policies() const;
  // POST /api/simulate. Throws ApiError(400) for invalid input. Blocks until every run is done.
  nlohmann::json simulate(const nlohmann::json& body) const;

  // The validation step of simulate(), exposed for tests.
  SimulateRequest parse_simulate(const nlohmann::json& body) const;

  bool model_available() const { return model_error_.empty(); }
  const std::string& model_error() const { return model_error_; }

 private:
  PolicyConfig config_for(std::uint64_t capacity_bytes) const;

  ApiOptions options_;
  PolicyConfig base_config_;  // model settings for learned / guarded
  std::string model_error_;   // why learned / guarded are unavailable; empty if they are
};

}  // namespace lcache::api
