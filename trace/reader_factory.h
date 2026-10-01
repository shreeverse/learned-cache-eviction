#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "trace/trace_reader.h"

namespace lcache {

// Formats: "oracle_general", "csv", or "auto" (picked from the file extension: .csv -> csv,
// anything else -> oracle_general). Throws std::invalid_argument for an unknown format.
std::unique_ptr<TraceReader> make_reader(std::string_view format,
                                         const std::filesystem::path& path);

}  // namespace lcache
