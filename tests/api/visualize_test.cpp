#include "api/visualize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "api/api.h"
#include "policy/belady.h"
#include "policy/lru.h"
#include "policy/policy_factory.h"
#include "sim/cache.h"
#include "sim/simulator.h"
#include "test_util.h"
#include "trace/csv_reader.h"
#include "trace/next_access.h"
#include "trace/vector_reader.h"

namespace lcache::api {
namespace {

using nlohmann::json;
using Slots = std::vector<std::optional<Key>>;

const std::string kTinyModel = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";

Request req(Key k) { return Request{.key = k, .size = 1}; }

// ---- key_label ----------------------------------------------------------------------------

TEST(KeyLabel, BijectiveBase26) {
  EXPECT_EQ(key_label(0), "A");
  EXPECT_EQ(key_label(25), "Z");
  EXPECT_EQ(key_label(26), "AA");
  EXPECT_EQ(key_label(27), "AB");
  EXPECT_EQ(key_label(51), "AZ");
  EXPECT_EQ(key_label(52), "BA");
  EXPECT_EQ(key_label(701), "ZZ");
  EXPECT_EQ(key_label(702), "AAA");
}

// ---- RecordingPolicy + SlotMap ------------------------------------------------------------

// Replays `trace` at a capacity of `slots` objects and returns the slots after each request.
std::vector<Slots> replay(RecordingPolicy& rec, const std::vector<Request>& trace,
                          std::size_t slots, std::vector<std::vector<Key>>* victims = nullptr) {
  Cache cache(rec, slots);
  SlotMap map(slots);
  std::vector<Slots> out;
  for (const auto& r : trace) {
    rec.clear();
    cache.access(r);
    map.apply(rec);
    out.push_back(map.slots());
    if (victims != nullptr) victims->push_back(rec.evicted());
  }
  return out;
}

TEST(SlotMap, LruKeepsUnchangedKeysInPlace) {
  RecordingPolicy rec(std::make_unique<LruPolicy>());
  std::vector<std::vector<Key>> victims;
  const auto steps = replay(rec, {req(1), req(2), req(1), req(3), req(2)}, 2, &victims);
  EXPECT_EQ(steps[0], (Slots{1, std::nullopt}));
  EXPECT_EQ(steps[1], (Slots{1, 2}));
  EXPECT_EQ(steps[2], (Slots{1, 2}));  // hit
  EXPECT_EQ(steps[3], (Slots{1, 3}));  // 2 is least recent; 3 takes its slot, 1 stays put
  EXPECT_EQ(steps[4], (Slots{2, 3}));  // 1 evicted; 2 takes the freed slot 0
  EXPECT_EQ(victims[3], (std::vector<Key>{2}));
  EXPECT_EQ(victims[4], (std::vector<Key>{1}));
  EXPECT_TRUE(victims[2].empty());
}

TEST(SlotMap, NewcomerEvictedAtOnceNeverGetsASlot) {
  // Belady at 2 slots: at request 3 (key 3), 3 is never requested again, so Belady evicts the
  // newcomer itself (decisions D2) and the slots don't change.
  VectorReader reader({req(1), req(2), req(1), req(3), req(1), req(2)});
  const auto trace = annotate_next_access(reader);
  RecordingPolicy rec(std::make_unique<BeladyPolicy>());
  std::vector<std::vector<Key>> victims;
  const auto steps = replay(rec, trace, 2, &victims);
  EXPECT_EQ(victims[3], (std::vector<Key>{3}));
  EXPECT_EQ(steps[3], (Slots{1, 2}));
  EXPECT_EQ(steps[5], (Slots{1, 2}));  // and 2 is a hit at the end
}

TEST(SlotMap, FindAndInconsistentEventsThrow) {
  RecordingPolicy rec(std::make_unique<LruPolicy>());
  Cache cache(rec, 1);
  SlotMap map(1);
  rec.clear();
  cache.access(req(7));
  map.apply(rec);
  EXPECT_EQ(map.find(7), std::optional<std::size_t>{0});
  EXPECT_EQ(map.find(8), std::nullopt);

  SlotMap too_small(0);  // the cache inserted a key the map has no room for
  EXPECT_THROW(too_small.apply(rec), std::logic_error);

  rec.clear();
  cache.access(req(9));  // evicts 7, inserts 9
  SlotMap empty(1);      // never saw 7
  EXPECT_THROW(empty.apply(rec), std::logic_error);
}

TEST(RecordingPolicy, ForwardsNameAndFutureKnowledge) {
  const RecordingPolicy lru(std::make_unique<LruPolicy>());
  const RecordingPolicy belady(std::make_unique<BeladyPolicy>());
  EXPECT_EQ(lru.name(), "lru");
  EXPECT_FALSE(lru.needs_next_access());
  EXPECT_TRUE(belady.needs_next_access());
}

// ---- Api::visualize_window ----------------------------------------------------------------

class VisualizeWindow : public ::testing::Test {
 protected:
  void SetUp() override {
    // Keys 10, 20, 10, 30, 20, then a window of 10 more requests.
    dir_.write("t.csv",
               "0,10,5\n1,20,5\n2,10,5\n3,30,5\n4,20,5\n"
               "5,10,5\n6,40,5\n7,10,5\n8,20,5\n9,50,5\n10,10,5\n11,40,5\n12,20,5\n13,60,5\n"
               "14,10,5\n");
  }

  const Api& api() const { return api_; }
  const std::filesystem::path& dir() const { return dir_.path(); }

  void expect_400(const json& body, const std::string& message_part) const {
    try {
      api_.parse_window(body);
      ADD_FAILURE() << "accepted: " << body.dump();
    } catch (const ApiError& e) {
      EXPECT_EQ(e.status(), 400);
      EXPECT_NE(std::string(e.what()).find(message_part), std::string::npos)
          << "message: " << e.what();
    }
  }

 private:
  test::TempDir dir_;
  Api api_{{.data_dir = dir_.path(), .model_path = kTinyModel}};
};

TEST_F(VisualizeWindow, LruStepsWithWarmup) {
  const json out = api().visualize_window(
      {{"trace", "t.csv"}, {"policies", {"lru"}}, {"slots", 2}, {"start", 5}, {"length", 10}});
  EXPECT_EQ(out["start"], 5);
  EXPECT_EQ(out["slots"], 2);
  EXPECT_EQ(out["length"], 10);

  // Warmup 10 20 10 30 20 at 2 slots (LRU): [10] [10 20] hit [10 30] [20 30].
  // Labels: the initial contents first (20 -> A, 30 -> B), then new keys in the window.
  const json& p = out["policies"][0];
  EXPECT_EQ(p["policy"], "lru");
  EXPECT_EQ(p["initial"], json({"A", "B"}));
  EXPECT_EQ(out["keys"]["A"], "20");
  EXPECT_EQ(out["keys"]["B"], "30");
  EXPECT_EQ(out["keys"]["C"], "10");
  EXPECT_EQ(out["requests"],
            json({"C", "D", "C", "A", "E", "C", "D", "A", "F", "C"}));  // 10 40 10 20 50 ...

  // Recency after the warmup, oldest first: 30, 20.
  const json& s = p["steps"];
  ASSERT_EQ(s.size(), 10U);
  // 10: miss, evicts 30 (least recent); 10 takes the freed slot 1 -> [A C]
  EXPECT_FALSE(s[0]["hit"].get<bool>());
  EXPECT_EQ(s[0]["evicted"], json({"B"}));
  EXPECT_EQ(s[0]["slots"], json({"A", "C"}));
  EXPECT_EQ(s[0]["slot"], 1);
  // 40: miss, evicts 20 -> [D C]
  EXPECT_EQ(s[1]["evicted"], json({"A"}));
  EXPECT_EQ(s[1]["slots"], json({"D", "C"}));
  // 10: hit, nothing moves
  EXPECT_TRUE(s[2]["hit"].get<bool>());
  EXPECT_EQ(s[2]["evicted"], json::array());
  EXPECT_EQ(s[2]["slots"], json({"D", "C"}));
  EXPECT_EQ(s[2]["slot"], 1);
  // From here every request misses (each key returns only after two others): 1 hit in all.
  EXPECT_EQ(s[9]["slots"], json({"C", "F"}));
  EXPECT_EQ(p["hits"], 1);
}

TEST_F(VisualizeWindow, HitsMatchSimulateForEveryPolicy) {
  for (const std::string policy : {"lru", "lfu", "arc", "wtinylfu", "belady", "learned"}) {
    SCOPED_TRACE(policy);
    const json out = api().visualize_window(
        {{"trace", "t.csv"}, {"policies", {policy}}, {"slots", 2}, {"start", 5}, {"length", 10}});
    CsvReader reader(dir() / "t.csv");
    const auto p =
        make_policy(policy, PolicyConfig{.capacity_bytes = 2,
                                         .model_path = policy == "learned" ? kTinyModel : "",
                                         .sample_size = 16});
    const SimResult r = simulate(
        reader, *p,
        {.capacity_bytes = 2, .ignore_size = true, .max_requests = 15, .warmup_requests = 5});
    EXPECT_EQ(out["policies"][0]["hits"], r.hits);
    std::uint64_t step_hits = 0;
    for (const auto& step : out["policies"][0]["steps"])
      step_hits += step["hit"].get<bool>() ? 1U : 0U;
    EXPECT_EQ(step_hits, r.hits);
  }
}

TEST_F(VisualizeWindow, SeveralPoliciesShareLabelsAndSlotsStayConsistent) {
  const json out = api().visualize_window(
      {{"trace", "t.csv"}, {"policies", {"lru", "belady", "arc"}}, {"slots", 3}, {"length", 15}});
  ASSERT_EQ(out["policies"].size(), 3U);
  EXPECT_EQ(out["policies"][1]["policy"], "belady");
  for (const auto& p : out["policies"]) {
    EXPECT_EQ(p["initial"], json({nullptr, nullptr, nullptr}));  // start 0: cold cache
    for (std::size_t i = 0; i < p["steps"].size(); ++i) {
      const json& step = p["steps"][i];
      const std::string key = out["requests"][i];
      ASSERT_EQ(step["slots"].size(), 3U);
      if (step["hit"].get<bool>() || !step["slot"].is_null()) {
        EXPECT_EQ(step["slots"][step["slot"].get<std::size_t>()], key);  // the accessed key
      }
      for (const auto& label : step["slots"]) {
        if (!label.is_null()) {
          EXPECT_TRUE(out["keys"].contains(label.get<std::string>()));
        }
      }
    }
  }
  // Belady (offline optimum) is never behind LRU.
  EXPECT_GE(out["policies"][1]["hits"], out["policies"][0]["hits"]);
}

TEST_F(VisualizeWindow, Validation) {
  const json ok = {{"trace", "t.csv"}, {"policies", {"lru"}}};
  const auto req = api().parse_window(ok);
  EXPECT_EQ(req.slots, WindowLimits::kDefaultSlots);
  EXPECT_EQ(req.start, 0U);
  EXPECT_EQ(req.length, 15U);  // the default 60 is cut to the trace's 15 requests

  json body = ok;
  body["policies"] = {"lru", "lfu", "arc", "wtinylfu", "belady"};
  expect_400(body, "more than 4");
  body = ok;
  body["slots"] = 1;
  expect_400(body, "'slots'");
  body["slots"] = 17;
  expect_400(body, "'slots'");
  body = ok;
  body["length"] = 9;
  expect_400(body, "'length'");
  body["length"] = 101;
  expect_400(body, "'length'");
  body = ok;
  body["start"] = 1'000'001;
  expect_400(body, "'start'");
  body["start"] = 6;  // 15 requests: a window of 10 must start at 5 at the latest
  body["length"] = 10;
  expect_400(body, "'start'");
  body = ok;
  body["cache_sizes"] = {4096};
  expect_400(body, "unknown field 'cache_sizes'");
  body = ok;
  body["trace"] = "../t.csv";
  expect_400(body, "unknown trace");
}

}  // namespace
}  // namespace lcache::api
