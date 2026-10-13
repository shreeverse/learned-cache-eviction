// Integration tests: the real routes on a real socket (a free port on 127.0.0.1), called with
// httplib::Client.

#include <gtest/gtest.h>
#include <httplib.h>

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "api/api.h"
#include "api/routes.h"
#include "test_util.h"

namespace lcache::api {
namespace {

using nlohmann::json;

const std::string kTinyModel = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";

class HttpApi : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string csv;
    for (int i = 0; i < 200; ++i) {
      csv += std::to_string(i) + "," + std::to_string(i % 13) + ",100\n";
    }
    dir_.write("t.csv", csv);
    api_ = std::make_unique<Api>(ApiOptions{.data_dir = dir_.path(), .model_path = kTinyModel});
    register_routes(server_, *api_, busy_, nullptr);
    port_ = server_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port_, 0);
    listener_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  void TearDown() override {
    server_.stop();
    if (listener_.joinable()) listener_.join();
  }

  httplib::Client client() const {
    httplib::Client c("127.0.0.1", port_);
    c.set_read_timeout(30, 0);
    return c;
  }

  // Sends a JSON POST and checks the status; returns the parsed body.
  json post(const std::string& path, const std::string& body, int status) const {
    auto res = client().Post(path, body, "application/json");
    EXPECT_TRUE(res) << "no response: " << httplib::to_string(res.error());
    if (!res) return nullptr;
    EXPECT_EQ(res->status, status) << res->body;
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
    return json::parse(res->body);
  }

  json get(const std::string& path, int status) const {
    auto res = client().Get(path);
    EXPECT_TRUE(res) << "no response: " << httplib::to_string(res.error());
    if (!res) return nullptr;
    EXPECT_EQ(res->status, status) << res->body;
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
    return json::parse(res->body);
  }

  std::mutex& busy() { return busy_; }

 private:
  test::TempDir dir_;
  std::unique_ptr<Api> api_;
  std::mutex busy_;
  httplib::Server server_;
  int port_ = 0;
  std::thread listener_;
};

TEST_F(HttpApi, Health) { EXPECT_EQ(get("/api/health", 200), json({{"status", "ok"}})); }

TEST_F(HttpApi, TracesAndPolicies) {
  const json traces = get("/api/traces", 200);
  ASSERT_EQ(traces.size(), 1U);
  EXPECT_EQ(traces[0]["name"], "t.csv");
  EXPECT_EQ(traces[0]["requests"], 200);
  const json policies = get("/api/policies", 200);
  EXPECT_EQ(policies.size(), 7U);
}

TEST_F(HttpApi, Simulate) {
  const json out = post("/api/simulate",
                        R"({"trace": "t.csv", "policies": ["lru", "belady"],
                            "cache_sizes": [1024, 2048], "series_points": 10})",
                        200);
  ASSERT_EQ(out["results"].size(), 4U);
  EXPECT_EQ(out["results"][0]["requests"], 200);
  EXPECT_EQ(out["results"][0]["series"]["hit_rate"].size(), 10U);
}

TEST_F(HttpApi, VisualizeWindow) {
  const json out = post(
      "/api/visualize-window",
      R"({"trace": "t.csv", "policies": ["lru", "learned"], "slots": 4, "start": 50, "length": 20})",
      200);
  EXPECT_EQ(out["requests"].size(), 20U);
  ASSERT_EQ(out["policies"].size(), 2U);
  EXPECT_EQ(out["policies"][1]["steps"].size(), 20U);
  // 13 keys in a loop and 4 slots: LRU always evicts the key needed next, so it never hits.
  EXPECT_EQ(out["policies"][0]["hits"], 0);
}

TEST_F(HttpApi, ClientErrorsAreJson) {
  for (const std::string path : {"/api/simulate", "/api/visualize-window"}) {
    SCOPED_TRACE(path);
    EXPECT_EQ(post(path, "{not json", 400)["error"], "body is not valid JSON");
    EXPECT_EQ(post(path, R"({"trace": "../t.csv", "policies": ["lru"]})", 400)["error"],
              "unknown trace '../t.csv'");
  }
  EXPECT_EQ(get("/api/nope", 404)["error"], "Not Found: /api/nope");
  // GET on a POST-only route is not routed either.
  EXPECT_EQ(get("/api/simulate", 404)["error"], "Not Found: /api/simulate");
}

TEST_F(HttpApi, OversizedBodyIs413) {
  const json out = post("/api/simulate", std::string(std::size_t{100} * 1024, ' '), 413);
  EXPECT_EQ(out["error"], "Payload Too Large: /api/simulate");
}

TEST_F(HttpApi, HeavyRequestsGet429WhileBusy) {
  const std::lock_guard hold(busy());  // as if a simulation were running
  const std::string body = R"({"trace": "t.csv", "policies": ["lru"], "cache_sizes": [1024]})";
  EXPECT_EQ(post("/api/simulate", body, 429)["error"],
            "a simulation is already running; try again shortly");
  post("/api/visualize-window", R"({"trace": "t.csv", "policies": ["lru"]})", 429);
  get("/api/traces", 200);  // light routes are not blocked
}

}  // namespace
}  // namespace lcache::api
