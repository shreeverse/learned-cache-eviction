// The training-data collector: LearnedPolicy + RecordingPredictor(AgePredictor).

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ml/feature_tracker.h"
#include "ml/recording_predictor.h"
#include "policy/learned.h"
#include "reference/workloads.h"
#include "sim/simulator.h"
#include "trace/vector_reader.h"

namespace lcache {
namespace {

using FT = FeatureTracker;

struct Row {
  std::uint64_t eviction = 0;
  std::uint64_t now = 0;
  Key key = 0;
  std::vector<double> features;
  std::int64_t next_distance = 0;
};

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> cells;
  std::stringstream ss(line);
  std::string cell;
  while (std::getline(ss, cell, ',')) cells.push_back(cell);
  return cells;
}

// Parses the CSV; the header is returned separately.
std::vector<Row> parse(const std::string& csv, std::string& header) {
  std::stringstream in(csv);
  std::getline(in, header);
  std::vector<Row> rows;
  std::string line;
  while (std::getline(in, line)) {
    const auto c = split(line);
    EXPECT_EQ(c.size(), 4 + FT::kNumFeatures) << line;
    Row r{.eviction = std::stoull(c[0]),
          .now = std::stoull(c[1]),
          .key = std::stoull(c[2]),
          .features = {},
          .next_distance = std::stoll(c.back())};
    for (std::size_t j = 0; j < FT::kNumFeatures; ++j) r.features.push_back(std::stod(c[3 + j]));
    rows.push_back(std::move(r));
  }
  return rows;
}

struct Collected {
  std::string csv;
  std::uint64_t evictions = 0;
  std::uint64_t rows = 0;
};

Collected collect(const std::vector<Key>& keys, std::uint64_t capacity, std::uint32_t sample,
                  std::uint32_t every) {
  std::vector<Request> reqs;
  for (const Key k : keys) reqs.push_back(test::req(k));
  VectorReader reader(std::move(reqs));
  std::ostringstream out;
  auto recorder =
      std::make_unique<RecordingPredictor>(std::make_unique<AgePredictor>(), out, every);
  const RecordingPredictor* stats = recorder.get();
  LearnedPolicy collector(std::move(recorder), "collector", {.sample_size = sample});
  EXPECT_TRUE(collector.needs_next_access());
  simulate(reader, collector, {.capacity_bytes = capacity});
  return {.csv = out.str(), .evictions = stats->evictions(), .rows = stats->rows_written()};
}

TEST(Collector, HeaderNamesEveryColumn) {
  std::ostringstream out;
  const RecordingPredictor rec(std::make_unique<AgePredictor>(), out);
  std::string expected = "eviction,now,key";
  for (const auto name : FT::feature_names()) expected += "," + std::string(name);
  EXPECT_EQ(out.str(), expected + ",next_distance\n");
}

TEST(Collector, NormalizedHeaderAndModelMismatch) {
  std::ostringstream out;
  const RecordingPredictor rec(std::make_unique<AgePredictor>(), out, 1, /*normalized=*/true);
  EXPECT_NE(out.str().find(",age_norm,"), std::string::npos) << out.str();
  EXPECT_EQ(rec.normalized_features(), true);
}

TEST(Collector, RejectsBadArguments) {
  std::ostringstream out;
  EXPECT_THROW(RecordingPredictor(nullptr, out), std::invalid_argument);
  EXPECT_THROW(RecordingPredictor(std::make_unique<AgePredictor>(), out, 0), std::invalid_argument);
}

TEST(Collector, LabelsAndFeaturesMatchABruteForceScanOfTheTrace) {
  const auto keys = test::zipf_keys(3'000, 100, 0.8, 21);
  const auto c = collect(keys, 20, 8, 1);
  std::string header;
  const auto rows = parse(c.csv, header);

  // Unit sizes and more cached keys (21 after the insert) than the sample size: 8 rows each.
  ASSERT_GT(c.evictions, 100U);
  EXPECT_EQ(c.rows, c.evictions * 8);
  ASSERT_EQ(rows.size(), c.rows);

  for (std::size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    ASSERT_EQ(r.eviction, i / 8);  // every eviction recorded, 8 candidates each
    ASSERT_LT(r.now, keys.size());
    // Label: distance to the key's next request after `now`, or -1.
    std::int64_t expected = -1;
    for (std::size_t t = r.now + 1; t < keys.size(); ++t) {
      if (keys[t] == r.key) {
        expected = static_cast<std::int64_t>(t - r.now);
        break;
      }
    }
    ASSERT_EQ(r.next_distance, expected) << "key " << r.key << " at " << r.now;

    // Age: distance back to the key's latest request at or before `now`.
    std::size_t last = r.now;
    while (keys[last] != r.key) --last;
    ASSERT_EQ(r.features[FT::kAge], static_cast<double>(r.now - last));
  }
}

TEST(Collector, EveryNthEvictionIsRecorded) {
  const auto keys = test::zipf_keys(3'000, 100, 0.8, 22);
  const auto all = collect(keys, 20, 8, 1);
  const auto third = collect(keys, 20, 8, 3);
  EXPECT_EQ(third.evictions, all.evictions);  // recording never changes what is evicted
  EXPECT_EQ(third.rows, (all.evictions + 2) / 3 * 8);
  std::string header;
  const auto rows = parse(third.csv, header);
  for (const Row& r : rows) ASSERT_EQ(r.eviction % 3, 0U);
}

TEST(Collector, NanFeaturesRoundTrip) {
  // A key seen once has no gaps; the CSV must carry those as "nan" for pandas/LightGBM.
  const auto c = collect({1, 2, 3}, 2, 8, 1);
  std::string header;
  const auto rows = parse(c.csv, header);
  ASSERT_FALSE(rows.empty());
  EXPECT_TRUE(std::isnan(rows[0].features[FT::kDelta0]));
}

}  // namespace
}  // namespace lcache
