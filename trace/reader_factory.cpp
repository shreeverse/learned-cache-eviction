#include "trace/reader_factory.h"

#include <stdexcept>
#include <string>

#include "trace/csv_reader.h"
#include "trace/oracle_general_reader.h"

namespace lcache {

std::unique_ptr<TraceReader> make_reader(std::string_view format,
                                         const std::filesystem::path& path) {
  if (format == "auto") {
    format = path.extension() == ".csv" ? "csv" : "oracle_general";
  }
  if (format == "oracle_general") return std::make_unique<OracleGeneralReader>(path);
  if (format == "csv") return std::make_unique<CsvReader>(path);
  throw std::invalid_argument("unknown trace format '" + std::string(format) +
                              "' (expected oracle_general, csv or auto)");
}

}  // namespace lcache
