#include "trace/csv_reader.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trace/hash.h"

namespace lcache {

namespace {

std::string_view trim(std::string_view s) {
  const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> split(std::string_view line, char sep) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = line.find(sep, start);
    fields.push_back(trim(line.substr(start, end - start)));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return fields;
}

// Parses the whole field as an unsigned integer; false if it isn't one or doesn't fit in T.
template <typename T>
bool parse_uint(std::string_view s, T& out) {
  const char* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), last, out);
  return !s.empty() && ec == std::errc{} && ptr == last;
}

Op parse_op(std::string_view s, bool& ok) {
  std::string lower(s);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  ok = true;
  if (lower == "get") return Op::Get;
  if (lower == "set") return Op::Set;
  if (lower == "del") return Op::Del;
  ok = false;
  return Op::Get;
}

}  // namespace

CsvReader::CsvReader(std::filesystem::path path) : path_(std::move(path)) {
  in_.open(path_);
  if (!in_) {
    throw std::runtime_error("cannot open trace file: " + path_.string());
  }
}

bool CsvReader::next(Request& out) {
  std::string line;
  while (std::getline(in_, line)) {
    ++line_no_;
    const std::string_view content = trim(line);
    if (content.empty() || content.front() == '#') continue;

    const auto fail = [&](const std::string& why) {
      throw std::runtime_error(path_.string() + ":" + std::to_string(line_no_) + ": " + why);
    };

    const auto fields = split(content, ',');
    if (fields.size() < 3 || fields.size() > 4) fail("expected time,key,size[,op]");

    Request r;
    if (!parse_uint(fields[0], r.time)) fail("bad time '" + std::string(fields[0]) + "'");
    if (fields[1].empty()) fail("empty key");
    if (!parse_uint(fields[1], r.key)) r.key = fnv1a64(fields[1]);
    if (!parse_uint(fields[2], r.size)) fail("bad size '" + std::string(fields[2]) + "'");
    if (fields.size() == 4) {
      bool ok = false;
      r.op = parse_op(fields[3], ok);
      if (!ok) fail("bad op '" + std::string(fields[3]) + "'");
    }
    out = r;
    return true;
  }
  return false;
}

void CsvReader::reset() {
  in_.clear();
  in_.seekg(0);
  line_no_ = 0;
}

}  // namespace lcache
