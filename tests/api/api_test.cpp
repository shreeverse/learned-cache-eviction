#include "api/api.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "policy/policy_factory.h"
#include "sim/simulator.h"
#include "test_util.h"
#include "trace/csv_reader.h"

namespace lcache::api {
namespace {

using nlohmann::json;

const std::string kTinyModel = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";

// A deterministic CSV trace with some reuse: keys drawn from 0..keys-1, sizes 100..1099 bytes
// fixed per key.
std::string make_csv(std::uint32_t n, std::uint32_t keys) {
  std::string out;
  std::uint64_t x = 12345;
  for (std::uint32_t i = 0; i < n; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t key = (x >> 33) % keys;
    // Squaring skews popularity towards small keys, so there are hits.
    const std::uint64_t skewed = key * key / keys;
    out += std::to_string(i) + "," + std::to_string(skewed) + "," +
           std::to_string(100 + (skewed * 37) % 1000) + "\n";
  }
  return out;
}

// ---- list_traces --------------------------------------------------------------------------

TEST(ListTraces, ListsSupportedFilesSortedByName) {
  test::TempDir dir;
  dir.write("b.csv", "1,1,10\n2,2,10\n3,1,10\n");
  dir.write("a.oracleGeneral.bin", std::string(48, '\0'));  // two 24-byte records
  dir.write("big.oracleGeneral.zst", "compressed");
  dir.write("notes.txt", "hello");
  dir.write("broken.csv", "not,a,number\n");
  std::filesystem::create_directory(dir.path() / "sub.csv");

  const auto traces = list_traces(dir.path());
  ASSERT_EQ(traces.size(), 2U);
  EXPECT_EQ(traces[0].name, "a.oracleGeneral.bin");
  EXPECT_EQ(traces[0].format, "oracle_general");
  EXPECT_EQ(traces[0].bytes, 48U);
  EXPECT_EQ(traces[0].requests, 2U);
  EXPECT_EQ(traces[1].name, "b.csv");
  EXPECT_EQ(traces[1].format, "csv");
  EXPECT_EQ(traces[1].requests, 3U);
}

TEST(ListTraces, ThrowsForMissingDirectory) {
  EXPECT_THROW(list_traces("/nonexistent/lcache"), std::filesystem::filesystem_error);
}

TEST(ApiTraces, JsonHasNoServerPaths) {
  test::TempDir dir;
  dir.write("t.csv", "1,1,10\n");
  const Api api({.data_dir = dir.path()});
  const json t = api.traces();
  ASSERT_EQ(t.size(), 1U);
  EXPECT_EQ(t[0]["name"], "t.csv");
  EXPECT_EQ(t[0]["requests"], 1);
  EXPECT_FALSE(t[0].contains("path"));
}

// ---- policies -----------------------------------------------------------------------------

json find_by_id(const json& list, const std::string& id) {
  for (const auto& p : list) {
    if (p["id"] == id) return p;
  }
  return nullptr;
}

TEST(ApiPolicies, ModelPoliciesUnavailableWithoutModel) {
  test::TempDir dir;
  const Api api({.data_dir = dir.path()});
  EXPECT_FALSE(api.model_available());
  const json p = api.policies();
  EXPECT_EQ(p.size(), 7U);
  EXPECT_TRUE(find_by_id(p, "lru")["available"].get<bool>());
  EXPECT_TRUE(find_by_id(p, "belady")["offline"].get<bool>());
  EXPECT_FALSE(find_by_id(p, "learned")["available"].get<bool>());
  EXPECT_FALSE(find_by_id(p, "guarded")["available"].get<bool>());
}

TEST(ApiPolicies, ModelPoliciesAvailableWithModel) {
  test::TempDir dir;
  const Api api({.data_dir = dir.path(), .model_path = kTinyModel});
  EXPECT_TRUE(api.model_available());
  EXPECT_TRUE(find_by_id(api.policies(), "learned")["available"].get<bool>());
}

TEST(ApiPolicies, BadModelFileMakesThemUnavailable) {
  test::TempDir dir;
  dir.write("bad_model.txt", "not a model");
  const Api api({.data_dir = dir.path(), .model_path = (dir.path() / "bad_model.txt").string()});
  EXPECT_FALSE(api.model_available());
  EXPECT_FALSE(find_by_id(api.policies(), "guarded")["available"].get<bool>());
}

// ---- parse_simulate -----------------------------------------------------------------------

class ParseSimulate : public ::testing::Test {
 protected:
  void SetUp() override { dir_.write("t.csv", make_csv(100, 20)); }

  // A valid body; tests change one field.
  static json valid() {
    return {{"trace", "t.csv"}, {"policies", {"lru"}}, {"cache_sizes", {4096}}};
  }

  const Api& api() const { return api_; }
  const std::filesystem::path& dir() const { return dir_.path(); }

  void expect_400(const json& body, const std::string& message_part) const {
    try {
      api_.parse_simulate(body);
      ADD_FAILURE() << "accepted: " << body.dump();
    } catch (const ApiError& e) {
      EXPECT_EQ(e.status(), 400);
      EXPECT_NE(std::string(e.what()).find(message_part), std::string::npos)
          << "message: " << e.what();
    }
  }

 private:
  test::TempDir dir_;
  Api api_{{.data_dir = dir_.path()}};
};

TEST_F(ParseSimulate, DefaultsAndNormalizedSizes) {
  json body = valid();
  body["cache_sizes"] = {65536, 4096, 65536};
  const auto req = api().parse_simulate(body);
  EXPECT_EQ(req.trace.name, "t.csv");
  EXPECT_EQ(req.policies, (std::vector<std::string>{"lru"}));
  EXPECT_EQ(req.cache_sizes, (std::vector<std::uint64_t>{4096, 65536}));
  EXPECT_EQ(req.max_requests, Limits::kDefaultRequests);
  EXPECT_EQ(req.warmup, 0U);
  EXPECT_EQ(req.series_points, Limits::kDefaultPoints);
}

TEST_F(ParseSimulate, RejectsBadBodiesAndTraces) {
  expect_400(json::array(), "JSON object");
  expect_400(json{{"policies", {"lru"}}, {"cache_sizes", {4096}}}, "'trace'");
  json body = valid();
  body["trace"] = "missing.csv";
  expect_400(body, "unknown trace");
  body["trace"] = "../" + dir().filename().string() + "/t.csv";  // same file via a path
  expect_400(body, "unknown trace");
  body["trace"] = (dir() / "t.csv").string();
  expect_400(body, "unknown trace");
  body = valid();
  body["cache_size"] = 4096;  // typo
  expect_400(body, "unknown field 'cache_size'");
}

TEST_F(ParseSimulate, RejectsBadPolicies) {
  json body = valid();
  body["policies"] = json::array();
  expect_400(body, "'policies' must be a non-empty array");
  body["policies"] = "lru";
  expect_400(body, "'policies' must be a non-empty array");
  body["policies"] = {"lru", 3};
  expect_400(body, "must contain strings");
  body["policies"] = {"mru"};
  expect_400(body, "unknown policy 'mru'");
  body["policies"] = {"oracle-sampled"};  // exists in the factory, not offered by the API
  expect_400(body, "unknown policy");
  body["policies"] = {"lru", "lru"};
  expect_400(body, "listed twice");
  body["policies"] = {"learned"};
  expect_400(body, "unavailable");
  body["policies"] = {"lru", "lfu", "arc", "wtinylfu", "belady", "learned", "guarded", "lru"};
  expect_400(body, "more than 7");
}

TEST_F(ParseSimulate, RejectsBadSizes) {
  json body = valid();
  body["cache_sizes"] = {1023};
  expect_400(body, "'cache_sizes'");
  body["cache_sizes"] = {(1ULL << 40) + 1};
  expect_400(body, "'cache_sizes'");
  body["cache_sizes"] = {-4096};
  expect_400(body, "'cache_sizes'");
  body["cache_sizes"] = {4096.5};
  expect_400(body, "'cache_sizes'");
  body["cache_sizes"] = {"4KiB"};
  expect_400(body, "'cache_sizes'");
  body["cache_sizes"] = json::array();
  for (int i = 0; i < 13; ++i) body["cache_sizes"].push_back(4096 + i);
  expect_400(body, "more than 12");
}

TEST_F(ParseSimulate, RejectsBadCounts) {
  json body = valid();
  body["max_requests"] = 0;
  expect_400(body, "'max_requests'");
  body["max_requests"] = Limits::kMaxRequests + 1;
  expect_400(body, "'max_requests'");
  body["max_requests"] = 50;
  body["warmup"] = 50;  // must leave at least one counted request
  expect_400(body, "'warmup'");
  body["warmup"] = 49;
  EXPECT_EQ(api().parse_simulate(body).warmup, 49U);
  body = valid();
  body["warmup"] = 100;  // the trace has 100 requests
  expect_400(body, "'warmup'");
  body = valid();
  body["series_points"] = 9;
  expect_400(body, "'series_points'");
  body["series_points"] = 1001;
  expect_400(body, "'series_points'");
}

// ---- simulate -----------------------------------------------------------------------------

SimResult direct(const std::filesystem::path& trace, const std::string& policy,
                 const PolicyConfig& config, const SimOptions& options) {
  CsvReader reader(trace);
  const auto p = make_policy(policy, config);
  return lcache::simulate(reader, *p, options);
}

TEST(ApiSimulate, MatchesDirectSimulation) {
  test::TempDir dir;
  dir.write("t.csv", make_csv(5000, 400));
  const Api api({.data_dir = dir.path(), .model_path = kTinyModel, .workers = 4});

  const json out = api.simulate({{"trace", "t.csv"},
                                 {"policies", {"lru", "wtinylfu", "belady", "learned"}},
                                 {"cache_sizes", {16384, 4096}},
                                 {"warmup", 1000},
                                 {"series_points", 30}});
  EXPECT_EQ(out["trace"], "t.csv");
  const std::uint64_t interval = out["series_interval"];
  EXPECT_EQ(interval, 134U);  // ceil(4000 / 30)
  ASSERT_EQ(out["results"].size(), 8U);

  std::size_t i = 0;
  for (const std::string policy : {"lru", "wtinylfu", "belady", "learned"}) {
    for (const std::uint64_t size : {4096U, 16384U}) {  // ascending, whatever the request order
      const json& r = out["results"][i++];
      SCOPED_TRACE(policy + " @ " + std::to_string(size));
      EXPECT_EQ(r["policy"], policy);
      EXPECT_EQ(r["cache_size"], size);
      const PolicyConfig config{.capacity_bytes = size,
                                .model_path = policy == "learned" ? kTinyModel : "",
                                .sample_size = 16};
      const SimResult d =
          direct(dir.path() / "t.csv", policy, config,
                 {.capacity_bytes = size, .warmup_requests = 1000, .series_interval = interval});
      EXPECT_EQ(r["requests"], d.requests);
      EXPECT_EQ(r["hits"], d.hits);
      EXPECT_EQ(r["bytes_requested"], d.bytes_requested);
      EXPECT_EQ(r["bytes_hit"], d.bytes_hit);
      EXPECT_DOUBLE_EQ(r["hit_rate"].get<double>(), d.hit_rate());
      ASSERT_EQ(r["series"]["hit_rate"].size(), d.series.size());
      EXPECT_EQ(r["series"]["hit_rate"].size(), 30U);
      EXPECT_EQ(r["series"]["byte_hit_rate"].size(), 30U);
      const auto& last = d.series.back();
      EXPECT_NEAR(r["series"]["hit_rate"].back().get<double>(),
                  static_cast<double>(last.hits) / static_cast<double>(last.requests), 1e-6);
    }
  }
  // Sanity: the trace has reuse, and Belady (offline optimum) beats LRU at the small size.
  EXPECT_GT(out["results"][0]["hits"], 0);
  EXPECT_GE(out["results"][4]["hits"], out["results"][0]["hits"]);
}

TEST(ApiSimulate, MaxRequestsLimitsTheReplay) {
  test::TempDir dir;
  dir.write("t.csv", make_csv(1000, 50));
  const Api api({.data_dir = dir.path()});
  const json out = api.simulate(
      {{"trace", "t.csv"}, {"policies", {"arc"}}, {"cache_sizes", {2048}}, {"max_requests", 300}});
  EXPECT_EQ(out["results"][0]["requests"], 300);
  EXPECT_EQ(out["series_interval"], 2);  // ceil(300 / 200)
  EXPECT_EQ(out["results"][0]["series"]["hit_rate"].size(), 150U);
}

}  // namespace
}  // namespace lcache::api
