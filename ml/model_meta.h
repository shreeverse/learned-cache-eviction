#pragma once

#include <optional>
#include <string>

#include "ml/feature_tracker.h"

namespace lcache {

// The FeatureConfig a training CSV was exported with, stored next to it as "<file>.meta" and
// copied by tools/train.py to "<model>.meta" (decisions D20). Closes the train/serve gap noted in
// D17: tools apply the model's settings instead of trusting the user to repeat them.
//
// Format, one per line: history=<n>, history_ratio=<x>, normalize=<0|1>.
void write_feature_meta(const std::string& file, const FeatureConfig& config);

// nullopt if "<file>.meta" doesn't exist; throws std::runtime_error if it is malformed.
std::optional<FeatureConfig> read_feature_meta(const std::string& file);

// The settings to run a model with. `given` holds the command line's values; `history_given` /
// `ratio_given` say whether the user set them explicitly. Settings from the model's .meta win
// over defaults; an explicit flag that contradicts them is an error (std::invalid_argument).
// Without a .meta file, `given` is returned unchanged.
FeatureConfig resolve_model_features(const std::string& model_path, FeatureConfig given,
                                     bool history_given, bool ratio_given);

}  // namespace lcache
