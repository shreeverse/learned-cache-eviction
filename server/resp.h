#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// RESP2, the Redis wire protocol (https://redis.io/docs/latest/develop/reference/protocol-spec/),
// restricted to what a server needs: parsing client commands and encoding replies. No Asio and
// no command semantics here (decisions D22); the connection code owns the buffers and the
// dispatcher interprets the arguments.
namespace lcache::resp {

// Bounds on what a client may send, so a malicious or broken client can't make the server buffer
// unbounded input. Exceeding one is a protocol error; the server replies and closes.
struct Limits {
  // Longest bulk string (one argument). Fits Request::size (uint32_t).
  std::size_t max_bulk_len = std::size_t{64} << 20;  // 64 MiB
  // Most arguments in one array command.
  std::size_t max_array_len = std::size_t{1} << 20;
  // A line (an inline command, or an array / bulk header) must end within this many bytes,
  // counting its "\n"; as in Redis, more bytes without a "\n" is an error.
  std::size_t max_inline_len = std::size_t{64} << 10;  // 64 KiB, as in Redis
};

enum class ParseStatus : std::uint8_t {
  Complete,  // one command parsed; `consumed` bytes may be dropped from the buffer
  NeedMore,  // buf holds a strict prefix of a valid command; call again with more bytes
  Error,     // buf can never become valid; reply with `error` and close the connection
};

struct ParseResult {
  ParseStatus status = ParseStatus::NeedMore;
  std::size_t consumed = 0;  // > 0 only for Complete
  // For Error: a message to send as a RESP error, without the leading '-' or the CRLF, e.g.
  // "ERR Protocol error: invalid bulk length". Points to a string literal.
  std::string_view error{};
};

// Parses the first command in buf. Two formats, chosen by the first byte:
//   - Array of bulk strings (first byte '*'), what redis-cli and redis-benchmark send:
//       "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n"  ->  args = {"GET", "key"}
//     Bulk strings are binary-safe (may contain "\r\n" or NUL). "*0\r\n" and "*-1\r\n" are
//     Complete with no arguments.
//   - Inline (any other first byte), for telnet / nc: one line ending in "\n" (an optional "\r"
//     before it is stripped), split on spaces and tabs:
//       "SET k v\r\n"  ->  args = {"SET", "k", "v"}
//     No quoting. A blank line is Complete with no arguments.
// The caller skips commands with no arguments, as Redis does.
//
// On Complete, `args` is replaced with views into buf (no copies): they stay valid only until
// the buffer is modified. On NeedMore or Error, `args` is unspecified. `args` is an out-parameter
// so the caller can reuse its capacity across commands.
//
// Stateless: after NeedMore, call again from the same start once more bytes have arrived.
// Re-parsing is cheap because bulk payloads are length-prefixed and never scanned; the cost per
// call is O(number of header lines) plus, for inline commands, O(line length).
//
// In the array format every line ends in "\r\n". Integers in headers are plain decimal: digits
// only, no leading zeros, '-' only in "*-1", no '+', no spaces, no overflow. A bulk string ("$-1"
// is not allowed in a request) must be followed by exactly "\r\n".
// Error messages start with "ERR Protocol error: ".
ParseResult parse_command(std::string_view buf, std::vector<std::string_view>& args,
                          const Limits& limits = {});

// ---- Reply encoding: each function appends one RESP value to `out`. ----------------------

// "+OK\r\n". `s` must not contain '\r' or '\n'.
void append_simple_string(std::string& out, std::string_view s);
// "-ERR unknown command\r\n" for msg = "ERR unknown command". `msg` must not contain '\r' or
// '\n'.
void append_error(std::string& out, std::string_view msg);
// ":42\r\n"
void append_integer(std::string& out, std::int64_t n);
// "$5\r\nhello\r\n". Binary-safe.
void append_bulk_string(std::string& out, std::string_view s);
// "$-1\r\n", the reply to a GET miss.
void append_null_bulk_string(std::string& out);
// "*3\r\n"; the caller then appends the n elements.
void append_array_header(std::string& out, std::size_t n);

}  // namespace lcache::resp
