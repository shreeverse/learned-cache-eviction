#include "ml/model_meta.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "test_util.h"

namespace lcache {
namespace {

// TempFile is the "model"; its sidecar is <path>.meta.
struct Model {
  test::TempFile file{".txt"};
  test::TempFile meta{".txt.meta"};  // same name + ".meta", so it is cleaned up too
  std::string path() const { return file.path().string(); }
};

TEST(ModelMeta, RoundTrip) {
  const Model m;
  const FeatureConfig c{.history = 0, .history_ratio = 2.5, .normalize = true};
  write_feature_meta(m.path(), c);
  EXPECT_EQ(read_feature_meta(m.path()), c);
}

TEST(ModelMeta, MissingIsNulloptAndMalformedThrows) {
  const Model m;
  EXPECT_EQ(read_feature_meta(m.path()), std::nullopt);
  m.meta.write("history=10\nnormalize=0\n");  // history_ratio missing
  EXPECT_THROW(read_feature_meta(m.path()), std::runtime_error);
  m.meta.write("history=ten\nhistory_ratio=0\nnormalize=0\n");
  EXPECT_THROW(read_feature_meta(m.path()), std::runtime_error);
  m.meta.write("history=1\nhistory_ratio=0\nnormalize=0\ncolor=blue\n");
  EXPECT_THROW(read_feature_meta(m.path()), std::runtime_error);
}

TEST(ModelMeta, ResolvePrefersTheModelsSettingsOverDefaults) {
  const Model m;
  // No meta: the command line's values stand.
  EXPECT_EQ(resolve_model_features(m.path(), {.history = 7}, false, false),
            (FeatureConfig{.history = 7}));

  write_feature_meta(m.path(), {.history = 0, .history_ratio = 2, .normalize = true});
  // Defaults are replaced by the model's settings.
  EXPECT_EQ(resolve_model_features(m.path(), {}, false, false),
            (FeatureConfig{.history = 0, .history_ratio = 2, .normalize = true}));
  // Explicit and equal: fine.
  EXPECT_NO_THROW(resolve_model_features(m.path(), {.history_ratio = 2}, false, true));
  // Explicit and different: refused, instead of running the model with the wrong window.
  EXPECT_THROW(resolve_model_features(m.path(), {.history_ratio = 3}, false, true),
               std::invalid_argument);
  EXPECT_THROW(resolve_model_features(m.path(), {.history = 10000}, true, false),
               std::invalid_argument);
}

}  // namespace
}  // namespace lcache
