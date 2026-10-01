#include "policy/policy_factory.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace lcache {
namespace {

TEST(PolicyFactory, EveryListedNameBuildsThatPolicy) {
  for (const std::string& name : policy_names()) {
    const auto policy = make_policy(name, PolicyConfig{.capacity_bytes = 1024});
    ASSERT_NE(policy, nullptr) << name;
    EXPECT_EQ(policy->name(), name);
  }
}

TEST(PolicyFactory, OnlyOracleAndBeladyNeedTheFuture) {
  for (const std::string& name : policy_names()) {
    const bool expected = name == "belady" || name == "oracle-sampled";
    EXPECT_EQ(make_policy(name, 1024)->needs_next_access(), expected) << name;
  }
}

TEST(PolicyFactory, RejectsUnknownNamesAndBadSampleSize) {
  EXPECT_THROW(make_policy("nope", 1024), std::invalid_argument);
  EXPECT_THROW(make_policy("oracle-sampled", PolicyConfig{.capacity_bytes = 1, .sample_size = 0}),
               std::invalid_argument);
}

TEST(PolicyFactory, GuardedWrapsTheLearnedPolicy) {
  const std::string model = std::string(LC_TEST_DATA_DIR) + "/tiny_model.txt";
  EXPECT_THROW(make_policy("guarded", 1024), std::invalid_argument);  // needs a model
  const auto p = make_policy("guarded", PolicyConfig{.capacity_bytes = 1024,
                                                     .model_path = model,
                                                     .guard_window = 500,
                                                     .budget_ns = 1'000'000});
  EXPECT_EQ(p->name(), "guarded");
  EXPECT_FALSE(p->needs_next_access());
}

}  // namespace
}  // namespace lcache
