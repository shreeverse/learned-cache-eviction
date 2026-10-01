// Regression test: our LRU must keep matching libCacheSim exactly on cloudPhysicsIO.
// Expected counts come from tools/validate_vs_libcachesim.sh (libCacheSim 93529a1); see
// docs/m1-validation.md. Skipped if the trace hasn't been downloaded (tools/fetch_traces.sh).

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

#include "policy/lru.h"
#include "sim/simulator.h"
#include "trace/oracle_general_reader.h"

namespace lcache {
namespace {

const std::filesystem::path kTrace =
    std::filesystem::path(LC_DATA_DIR) / "cloudPhysicsIO.oracleGeneral.bin";

struct Golden {
  std::uint64_t capacity;
  bool ignore_size;
  std::uint64_t hits;
  std::uint64_t bytes_hit;  // only checked when sizes are used
};

void PrintTo(const Golden& g, std::ostream* os) {
  *os << "{capacity=" << g.capacity << ", ignore_size=" << g.ignore_size << "}";
}

constexpr std::uint64_t kRequests = 113872;
constexpr std::uint64_t kBytesRequested = 4368040448;

class LibCacheSimGolden : public ::testing::TestWithParam<Golden> {};

TEST_P(LibCacheSimGolden, LruMatchesExactly) {
  if (!std::filesystem::exists(kTrace)) GTEST_SKIP() << "run tools/fetch_traces.sh first";
  const Golden g = GetParam();

  OracleGeneralReader reader(kTrace);
  LruPolicy lru;
  const auto r =
      simulate(reader, lru, {.capacity_bytes = g.capacity, .ignore_size = g.ignore_size});

  EXPECT_EQ(r.requests, kRequests);
  EXPECT_EQ(r.hits, g.hits);
  EXPECT_EQ(r.bytes_requested, kBytesRequested);
  if (!g.ignore_size) {
    EXPECT_EQ(r.bytes_hit, g.bytes_hit);
  }
}

INSTANTIATE_TEST_SUITE_P(CloudPhysicsIO, LibCacheSimGolden,
                         ::testing::Values(Golden{64ULL << 20, false, 19669, 110606336},
                                           Golden{256ULL << 20, false, 24089, 306798080},
                                           Golden{1ULL << 30, false, 42168, 1306377728},
                                           Golden{100, true, 13657, 0},
                                           Golden{1000, true, 19049, 0},
                                           Golden{10000, true, 34434, 0}),
                         [](const ::testing::TestParamInfo<Golden>& param_info) {
                           return std::to_string(param_info.param.capacity) +
                                  (param_info.param.ignore_size ? "_objects" : "_bytes");
                         });

}  // namespace
}  // namespace lcache
