#include "server/resp.h"

#include <array>
#include <charconv>
#include <optional>
#include <system_error>

namespace lcache::resp {
namespace {

constexpr ParseResult need_more() { return {}; }

constexpr ParseResult error(std::string_view msg) {
  return {.status = ParseStatus::Error, .consumed = 0, .error = msg};
}

constexpr ParseResult complete(std::size_t consumed) {
  return {.status = ParseStatus::Complete, .consumed = consumed, .error = {}};
}

enum class Scan : std::uint8_t { Ok, NeedMore, Invalid, TooLong };

// Finds the '\n' ending the line that starts at buf[pos]. The line, counting its '\n', must fit
// in max_line bytes; as in Redis, more than max_line bytes without a '\n' is TooLong, fewer is
// NeedMore.
struct LineEnd {
  Scan scan = Scan::NeedMore;
  std::size_t newline = 0;  // for Ok: the index of the '\n'
};

LineEnd find_line_end(std::string_view buf, std::size_t pos, std::size_t max_line) {
  const std::size_t offset = buf.substr(pos, max_line).find('\n');
  if (offset != std::string_view::npos) return {.scan = Scan::Ok, .newline = pos + offset};
  return {.scan = buf.size() - pos > max_line ? Scan::TooLong : Scan::NeedMore};
}

// A header integer: digits without leading zeros ("0" alone is fine), or "-1" if allowed.
std::optional<std::int64_t> parse_decimal(std::string_view s, bool allow_minus_one) {
  if (allow_minus_one && s == "-1") return -1;
  if (s.empty() || s[0] < '0' || s[0] > '9' || (s[0] == '0' && s.size() > 1)) return std::nullopt;
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;  // overflow, junk
  return value;
}

// True if `partial` (a header's bytes after the type byte, no '\n' yet) can still be completed
// into "<integer>\r". Lets the parser reject a bad header before its line ends: NeedMore
// promises a valid prefix.
bool could_complete_header(std::string_view partial, bool allow_minus_one) {
  if (partial.ends_with('\r')) {
    partial.remove_suffix(1);
    return parse_decimal(partial, allow_minus_one).has_value();
  }
  if (partial.empty() || (allow_minus_one && partial == "-")) return true;
  return parse_decimal(partial, allow_minus_one).has_value();
}

struct Header {
  Scan scan = Scan::NeedMore;
  std::int64_t value = 0;
  std::size_t next = 0;  // for Ok: the index just past the header's "\r\n"
};

// Parses an array or bulk header at buf[pos]: a type byte ('*' or '$', checked by the caller),
// a decimal integer, "\r\n".
Header parse_header(std::string_view buf, std::size_t pos, bool allow_minus_one,
                    std::size_t max_line) {
  const LineEnd end = find_line_end(buf, pos, max_line);
  if (end.scan == Scan::TooLong) return {.scan = Scan::TooLong};
  if (end.scan == Scan::NeedMore) {
    const bool ok = could_complete_header(buf.substr(pos + 1), allow_minus_one);
    return {.scan = ok ? Scan::NeedMore : Scan::Invalid};
  }
  std::string_view digits = buf.substr(pos + 1, end.newline - pos - 1);
  if (!digits.ends_with('\r')) return {.scan = Scan::Invalid};  // a bare "\n"
  digits.remove_suffix(1);
  const std::optional<std::int64_t> value = parse_decimal(digits, allow_minus_one);
  if (!value) return {.scan = Scan::Invalid};
  return {.scan = Scan::Ok, .value = *value, .next = end.newline + 1};
}

ParseResult parse_array(std::string_view buf, std::vector<std::string_view>& args,
                        const Limits& limits) {
  const Header count = parse_header(buf, 0, /*allow_minus_one=*/true, limits.max_inline_len);
  switch (count.scan) {
    case Scan::Ok:
      break;
    case Scan::NeedMore:
      return need_more();
    case Scan::Invalid:
      return error("ERR Protocol error: invalid multibulk length");
    case Scan::TooLong:
      return error("ERR Protocol error: too big mbulk count string");
  }
  // "*0" and "*-1": an empty command.
  if (count.value <= 0) return complete(count.next);
  if (static_cast<std::uint64_t>(count.value) > limits.max_array_len) {
    return error("ERR Protocol error: invalid multibulk length");
  }
  // No args.reserve(count.value): the count is untrusted and its elements may never arrive.

  std::size_t pos = count.next;
  for (std::int64_t k = 0; k < count.value; ++k) {
    if (pos == buf.size()) return need_more();
    if (buf[pos] != '$') return error("ERR Protocol error: expected '$'");

    const Header len = parse_header(buf, pos, /*allow_minus_one=*/false, limits.max_inline_len);
    switch (len.scan) {
      case Scan::Ok:
        break;
      case Scan::NeedMore:
        return need_more();
      case Scan::Invalid:
        return error("ERR Protocol error: invalid bulk length");
      case Scan::TooLong:
        return error("ERR Protocol error: too big bulk count string");
    }
    // Checked before the payload arrives, so an oversized value is never buffered.
    if (static_cast<std::uint64_t>(len.value) > limits.max_bulk_len) {
      return error("ERR Protocol error: invalid bulk length");
    }

    // The payload is not scanned: its length alone says where the "\r\n" must be. Each
    // terminator byte is checked as soon as it is in the buffer.
    const auto n = static_cast<std::size_t>(len.value);
    const std::size_t have = buf.size() - len.next;
    if (have > n && buf[len.next + n] != '\r') {
      return error("ERR Protocol error: expected CRLF after bulk string");
    }
    if (have > n + 1 && buf[len.next + n + 1] != '\n') {
      return error("ERR Protocol error: expected CRLF after bulk string");
    }
    if (have < n + 2) return need_more();

    args.push_back(buf.substr(len.next, n));
    pos = len.next + n + 2;
  }
  return complete(pos);
}

ParseResult parse_inline(std::string_view buf, std::vector<std::string_view>& args,
                         const Limits& limits) {
  const LineEnd end = find_line_end(buf, 0, limits.max_inline_len);
  if (end.scan == Scan::TooLong) return error("ERR Protocol error: too big inline request");
  if (end.scan == Scan::NeedMore) return need_more();

  std::string_view line = buf.substr(0, end.newline);
  if (line.ends_with('\r')) line.remove_suffix(1);
  constexpr std::string_view kBlanks = " \t";
  for (std::size_t start = line.find_first_not_of(kBlanks); start != std::string_view::npos;) {
    const std::size_t stop = line.find_first_of(kBlanks, start);
    args.push_back(line.substr(start, stop - start));  // stop == npos takes the rest
    if (stop == std::string_view::npos) break;
    start = line.find_first_not_of(kBlanks, stop);
  }
  return complete(end.newline + 1);
}

void append_crlf(std::string& out) { out.append("\r\n"); }

// "<prefix><n>\r\n" without a temporary std::string.
template <typename Int>
void append_number_line(std::string& out, char prefix, Int n) {
  std::array<char, 24> digits{};  // fits any 64-bit integer with its sign
  const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), n);
  (void)ec;  // cannot fail: the buffer is large enough
  out.push_back(prefix);
  out.append(digits.data(), end);
  append_crlf(out);
}

}  // namespace

ParseResult parse_command(std::string_view buf, std::vector<std::string_view>& args,
                          const Limits& limits) {
  args.clear();
  if (buf.starts_with('*')) return parse_array(buf, args, limits);
  return parse_inline(buf, args, limits);
}

void append_simple_string(std::string& out, std::string_view s) {
  out.push_back('+');
  out.append(s);
  append_crlf(out);
}

void append_error(std::string& out, std::string_view msg) {
  out.push_back('-');
  out.append(msg);
  append_crlf(out);
}

void append_integer(std::string& out, std::int64_t n) { append_number_line(out, ':', n); }

void append_bulk_string(std::string& out, std::string_view s) {
  append_number_line(out, '$', s.size());
  out.append(s);
  append_crlf(out);
}

void append_null_bulk_string(std::string& out) { out.append("$-1\r\n"); }

void append_array_header(std::string& out, std::size_t n) { append_number_line(out, '*', n); }

}  // namespace lcache::resp
