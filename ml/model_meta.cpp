#include "ml/model_meta.h"

#include <fstream>
#include <stdexcept>

namespace lcache {

void write_feature_meta(const std::string& file, const FeatureConfig& config) {
  std::ofstream out(file + ".meta");
  if (!out) throw std::runtime_error("cannot write " + file + ".meta");
  out << "history=" << config.history << "\nhistory_ratio=" << config.history_ratio
      << "\nnormalize=" << (config.normalize ? 1 : 0) << '\n';
}

std::optional<FeatureConfig> read_feature_meta(const std::string& file) {
  std::ifstream in(file + ".meta");
  if (!in) return std::nullopt;
  FeatureConfig c;
  int seen = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.empty()) continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) throw std::runtime_error(file + ".meta: bad line '" + line + "'");
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    try {
      if (key == "history") {
        c.history = std::stoull(value);
      } else if (key == "history_ratio") {
        c.history_ratio = std::stod(value);
      } else if (key == "normalize") {
        c.normalize = value == "1";
      } else {
        throw std::runtime_error(file + ".meta: unknown key '" + key + "'");
      }
    } catch (const std::logic_error&) {  // stoull / stod
      throw std::runtime_error(file + ".meta: bad value in '" + line + "'");
    }
    ++seen;
  }
  if (seen != 3)
    throw std::runtime_error(file + ".meta: expected history, history_ratio, normalize");
  return c;
}

FeatureConfig resolve_model_features(const std::string& model_path, FeatureConfig given,
                                     bool history_given, bool ratio_given) {
  const auto meta = read_feature_meta(model_path);
  if (!meta) return given;
  const auto conflict = [&](const std::string& what) {
    throw std::invalid_argument("--" + what + " contradicts " + model_path +
                                ".meta (the model was trained with other settings)");
  };
  if (history_given && given.history != meta->history) conflict("history");
  if (ratio_given && given.history_ratio != meta->history_ratio) conflict("history-ratio");
  FeatureConfig out = *meta;
  return out;
}

}  // namespace lcache
