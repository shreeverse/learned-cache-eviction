#include "api/api.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#include "ml/model_meta.h"
#include "sim/simulator.h"
#include "trace/csv_reader.h"
#include "trace/oracle_general_reader.h"
#include "trace/reader_factory.h"

namespace lcache::api {

namespace {

using nlohmann::json;

struct PolicyInfo {
  std::string_view id;
  std::string_view label;
  bool offline;
  bool needs_model;
  std::string_view note;
};

// What the dashboard offers. The sampled diagnostic policies (oracle-sampled, random-sampled) are
// left out.
constexpr std::array<PolicyInfo, 7> kPolicies{{
    {"lru", "LRU", false, false, ""},
    {"lfu", "LFU", false, false, ""},
    {"arc", "ARC", false, false, ""},
    {"wtinylfu", "W-TinyLFU", false, false, ""},
    {"belady", "Belady", true, false,
     "Offline: knows every future request; the reference to beat, not a real policy"},
    {"learned", "Learned", false, true,
     "LightGBM model trained on Twitter cluster26 at 1 MiB; may not transfer to other traces"},
    {"guarded", "Learned + fallback", false, true,
     "Learned, falling back to W-TinyLFU when it falls behind (decisions D21)"},
}};

const PolicyInfo* find_policy(std::string_view id) {
  for (const auto& p : kPolicies) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

constexpr std::string_view kOracleSuffix = ".oracleGeneral.bin";

[[noreturn]] void bad_request(const std::string& message) { throw ApiError(400, message); }

// A non-negative integer. JSON text parses positive integers as unsigned, but values built in
// C++ from an int are signed, so both are accepted.
std::optional<std::uint64_t> as_uint(const json& v) {
  if (v.is_number_unsigned()) return v.get<std::uint64_t>();
  if (v.is_number_integer() && v.get<std::int64_t>() >= 0) {
    return static_cast<std::uint64_t>(v.get<std::int64_t>());
  }
  return std::nullopt;
}

// An unsigned integer field: `fallback` if absent, else must be an integer in [lo, hi].
std::uint64_t get_uint(const json& body, const char* field, std::uint64_t fallback,
                       std::uint64_t lo, std::uint64_t hi) {
  const auto it = body.find(field);
  if (it == body.end()) return fallback;
  const auto v = as_uint(*it);
  if (!v || *v < lo || *v > hi) {
    bad_request(std::string("'") + field + "' must be an integer in [" + std::to_string(lo) + ", " +
                std::to_string(hi) + "]");
  }
  return *v;
}

const json& get_array(const json& body, const char* field, std::size_t max_items) {
  const auto it = body.find(field);
  if (it == body.end() || !it->is_array() || it->empty()) {
    bad_request(std::string("'") + field + "' must be a non-empty array");
  }
  if (it->size() > max_items) {
    bad_request(std::string("'") + field + "' has more than " + std::to_string(max_items) +
                " items");
  }
  return *it;
}

// Series values are rounded to 1e-6 (0.0001 pp) to keep the payload small; totals stay exact.
double round6(double x) { return std::round(x * 1e6) / 1e6; }

double ratio(std::uint64_t num, std::uint64_t den) {
  return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
}

json result_json(const std::string& policy, std::uint64_t cache_size, const SimResult& r,
                 std::uint64_t interval, double elapsed_ms) {
  json hit_rate = json::array();
  json byte_hit_rate = json::array();
  for (const auto& b : r.series) {
    hit_rate.push_back(round6(ratio(b.hits, b.requests)));
    byte_hit_rate.push_back(round6(ratio(b.bytes_hit, b.bytes_requested)));
  }
  return {{"policy", policy},
          {"cache_size", cache_size},
          {"requests", r.requests},
          {"hits", r.hits},
          {"hit_rate", r.hit_rate()},
          {"bytes_requested", r.bytes_requested},
          {"bytes_hit", r.bytes_hit},
          {"byte_hit_rate", r.byte_hit_rate()},
          {"elapsed_ms", elapsed_ms},
          {"series",
           {{"interval", interval}, {"hit_rate", hit_rate}, {"byte_hit_rate", byte_hit_rate}}}};
}

double ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

std::vector<TraceInfo> list_traces(const std::filesystem::path& dir) {
  std::vector<TraceInfo> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    TraceInfo t{.name = name,
                .format = {},
                .bytes = entry.file_size(),
                .requests = 0,
                .path = entry.path()};
    if (name.size() > kOracleSuffix.size() && name.ends_with(kOracleSuffix)) {
      t.format = "oracle_general";
      t.requests = t.bytes / OracleGeneralReader::kRecordSize;
    } else if (entry.path().extension() == ".csv") {
      // CSV traces are small hand-written ones, so counting them on each listing is cheap.
      t.format = "csv";
      try {
        CsvReader reader(entry.path());
        Request r;
        while (reader.next(r)) ++t.requests;
      } catch (const std::exception&) {
        continue;  // malformed: not offered
      }
    } else {
      continue;
    }
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(),
            [](const TraceInfo& a, const TraceInfo& b) { return a.name < b.name; });
  return out;
}

Api::Api(ApiOptions options) : options_(std::move(options)) {
  if (options_.workers == 0) options_.workers = 1;
  base_config_.sample_size = 16;  // models/README.md: within 0.62 pp of k = 64, ~4x faster
  if (options_.model_path.empty()) {
    model_error_ = "no model configured (start lcache-api with --model)";
    return;
  }
  try {
    // Run the model with the settings it was trained with (decisions D20).
    const auto f = resolve_model_features(options_.model_path, {}, false, false);
    base_config_.model_path = options_.model_path;
    base_config_.history = f.history;
    base_config_.history_ratio = f.history_ratio;
    make_policy("learned", config_for(1 << 20));  // fails now rather than in every simulation
  } catch (const std::exception& e) {
    model_error_ = e.what();
  }
}

PolicyConfig Api::config_for(std::uint64_t capacity_bytes) const {
  PolicyConfig c = base_config_;
  c.capacity_bytes = capacity_bytes;
  return c;
}

json Api::traces() const {
  json out = json::array();
  for (const auto& t : list_traces(options_.data_dir)) {
    out.push_back(
        {{"name", t.name}, {"format", t.format}, {"bytes", t.bytes}, {"requests", t.requests}});
  }
  return out;
}

json Api::policies() const {
  json out = json::array();
  for (const auto& p : kPolicies) {
    const bool available = !p.needs_model || model_available();
    std::string note(p.note);
    if (!available) note = "Unavailable: " + model_error_;
    out.push_back({{"id", p.id},
                   {"label", p.label},
                   {"offline", p.offline},
                   {"available", available},
                   {"note", note}});
  }
  return out;
}

SimulateRequest Api::parse_simulate(const json& body) const {
  if (!body.is_object()) bad_request("body must be a JSON object");
  static const std::set<std::string> kFields{"trace",        "policies", "cache_sizes",
                                             "max_requests", "warmup",   "series_points"};
  for (const auto& [key, value] : body.items()) {
    if (!kFields.contains(key)) bad_request("unknown field '" + key + "'");
  }

  SimulateRequest req;

  // The trace is looked up by name in the server's own listing, so a client-supplied path never
  // reaches the filesystem (decisions D26).
  const auto trace_it = body.find("trace");
  if (trace_it == body.end() || !trace_it->is_string()) bad_request("'trace' must be a string");
  const auto name = trace_it->get<std::string>();
  const auto traces = list_traces(options_.data_dir);
  const auto t = std::find_if(traces.begin(), traces.end(),
                              [&](const TraceInfo& info) { return info.name == name; });
  if (t == traces.end()) bad_request("unknown trace '" + name + "'");
  req.trace = *t;

  for (const auto& p : get_array(body, "policies", Limits::kMaxPolicies)) {
    if (!p.is_string()) bad_request("'policies' must contain strings");
    const auto id = p.get<std::string>();
    const PolicyInfo* info = find_policy(id);
    if (info == nullptr) bad_request("unknown policy '" + id + "'");
    if (info->needs_model && !model_available()) {
      bad_request("policy '" + id + "' is unavailable: " + model_error_);
    }
    if (std::find(req.policies.begin(), req.policies.end(), id) != req.policies.end()) {
      bad_request("policy '" + id + "' is listed twice");
    }
    req.policies.push_back(id);
  }

  for (const auto& s : get_array(body, "cache_sizes", Limits::kMaxSizes)) {
    const auto size = as_uint(s);
    if (!size || *size < Limits::kMinSize || *size > Limits::kMaxSize) {
      bad_request("'cache_sizes' must be integers (bytes) in [" + std::to_string(Limits::kMinSize) +
                  ", " + std::to_string(Limits::kMaxSize) + "]");
    }
    req.cache_sizes.push_back(*size);
  }
  std::sort(req.cache_sizes.begin(), req.cache_sizes.end());
  req.cache_sizes.erase(std::unique(req.cache_sizes.begin(), req.cache_sizes.end()),
                        req.cache_sizes.end());

  req.max_requests =
      get_uint(body, "max_requests", Limits::kDefaultRequests, 1, Limits::kMaxRequests);
  const std::uint64_t replayed = std::min(req.max_requests, req.trace.requests);
  if (replayed == 0) bad_request("trace '" + name + "' is empty");
  req.warmup = get_uint(body, "warmup", 0, 0, replayed - 1);
  req.series_points = static_cast<std::uint32_t>(get_uint(
      body, "series_points", Limits::kDefaultPoints, Limits::kMinPoints, Limits::kMaxPoints));
  return req;
}

json Api::simulate(const json& body) const {
  const auto start = std::chrono::steady_clock::now();
  const SimulateRequest req = parse_simulate(body);

  // Downsampling (decisions D26): exact counts per interval, at most series_points intervals.
  const std::uint64_t counted = std::min(req.max_requests, req.trace.requests) - req.warmup;
  const std::uint64_t interval =
      std::max<std::uint64_t>(1, (counted + req.series_points - 1) / req.series_points);

  struct Run {
    std::string policy;
    std::uint64_t cache_size;
  };
  std::vector<Run> runs;
  for (const auto& p : req.policies) {
    for (const auto size : req.cache_sizes) runs.push_back({p, size});
  }

  // Runs are independent: each worker takes the next index and writes only its own slot. Every
  // run gets its own reader (readers hold a file position) and a fresh policy.
  std::vector<json> results(runs.size());
  std::vector<std::exception_ptr> errors(runs.size());
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (std::size_t i = next.fetch_add(1); i < runs.size(); i = next.fetch_add(1)) {
      try {
        const auto run_start = std::chrono::steady_clock::now();
        const auto reader = make_reader(req.trace.format, req.trace.path);
        const auto policy = make_policy(runs[i].policy, config_for(runs[i].cache_size));
        const SimResult r = lcache::simulate(*reader, *policy,
                                             {.capacity_bytes = runs[i].cache_size,
                                              .max_requests = req.max_requests,
                                              .warmup_requests = req.warmup,
                                              .series_interval = interval});
        results[i] =
            result_json(runs[i].policy, runs[i].cache_size, r, interval, ms_since(run_start));
      } catch (...) {
        errors[i] = std::current_exception();
      }
    }
  };
  {
    const auto n = std::min<std::size_t>(options_.workers, runs.size());
    std::vector<std::jthread> workers;
    workers.reserve(n);
    for (std::size_t w = 0; w < n; ++w) workers.emplace_back(work);
  }  // jthreads join here
  for (const auto& e : errors) {
    if (e) std::rethrow_exception(e);
  }

  return {{"trace", req.trace.name},     {"max_requests", req.max_requests}, {"warmup", req.warmup},
          {"series_interval", interval}, {"elapsed_ms", ms_since(start)},    {"results", results}};
}

}  // namespace lcache::api
