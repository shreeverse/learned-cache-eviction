#include "server/resp.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace lcache::resp {
namespace {

using namespace std::string_literals;  // "..."s keeps embedded NULs

// parse_command() with the views copied out, so results outlive the buffer.
struct Parsed {
  ParseStatus status;
  std::size_t consumed;
  std::vector<std::string> args;
  std::string error;
};

Parsed parse(std::string_view buf, const Limits& limits = {}) {
  std::vector<std::string_view> views;
  const ParseResult r = parse_command(buf, views, limits);
  Parsed p{.status = r.status, .consumed = r.consumed, .args = {}, .error = std::string(r.error)};
  if (r.status == ParseStatus::Complete) p.args.assign(views.begin(), views.end());
  return p;
}

using Args = std::vector<std::string>;

void expect_complete(std::string_view buf, const Args& args) {
  SCOPED_TRACE(testing::Message() << "input: " << testing::PrintToString(std::string(buf)));
  const Parsed p = parse(buf);
  ASSERT_EQ(p.status, ParseStatus::Complete) << p.error;
  EXPECT_EQ(p.consumed, buf.size());
  EXPECT_EQ(p.args, args);
}

void expect_error(std::string_view buf, const Limits& limits = {}) {
  SCOPED_TRACE(testing::Message() << "input: " << testing::PrintToString(std::string(buf)));
  const Parsed p = parse(buf, limits);
  ASSERT_EQ(p.status, ParseStatus::Error);
  EXPECT_EQ(p.consumed, 0U);
  EXPECT_TRUE(p.error.starts_with("ERR Protocol error: ")) << p.error;
}

void expect_need_more(std::string_view buf, const Limits& limits = {}) {
  SCOPED_TRACE(testing::Message() << "input: " << testing::PrintToString(std::string(buf)));
  const Parsed p = parse(buf, limits);
  EXPECT_EQ(p.status, ParseStatus::NeedMore) << p.error;
  EXPECT_EQ(p.consumed, 0U);
}

// ---- Array format ------------------------------------------------------------------------

TEST(RespParse, ArrayGet) { expect_complete("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n", {"GET", "key"}); }

TEST(RespParse, ArraySet) {
  expect_complete("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n", {"SET", "k", "hello"});
}

TEST(RespParse, BulkStringsAreBinarySafe) {
  // The payload contains CRLF, NUL and a '$': only the length decides where it ends.
  expect_complete("*2\r\n$3\r\nSET\r\n$7\r\na\r\n\0$b\n\r\n"s, {"SET", "a\r\n\0$b\n"s});
}

TEST(RespParse, EmptyBulkString) { expect_complete("*2\r\n$3\r\nGET\r\n$0\r\n\r\n", {"GET", ""}); }

TEST(RespParse, MultiDigitLengths) {
  const std::string value(1234, 'v');
  expect_complete("*2\r\n$3\r\nSET\r\n$1234\r\n" + value + "\r\n", {"SET", value});
}

TEST(RespParse, EmptyAndNullArraysHaveNoArguments) {
  expect_complete("*0\r\n", {});
  expect_complete("*-1\r\n", {});
}

TEST(RespParse, ConsumesOnlyTheFirstOfPipelinedCommands) {
  const std::string first = "*2\r\n$3\r\nGET\r\n$1\r\na\r\n";
  const std::string second = "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n";
  const std::string buf = first + second;

  const Parsed p1 = parse(buf);
  ASSERT_EQ(p1.status, ParseStatus::Complete);
  EXPECT_EQ(p1.consumed, first.size());
  EXPECT_EQ(p1.args, (Args{"GET", "a"}));

  const Parsed p2 = parse(std::string_view(buf).substr(p1.consumed));
  ASSERT_EQ(p2.status, ParseStatus::Complete);
  EXPECT_EQ(p2.consumed, second.size());
  EXPECT_EQ(p2.args, (Args{"GET", "b"}));
}

TEST(RespParse, ArgumentsAreViewsIntoTheBuffer) {
  const std::string buf = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
  std::vector<std::string_view> args;
  ASSERT_EQ(parse_command(buf, args).status, ParseStatus::Complete);
  ASSERT_EQ(args.size(), 2U);
  EXPECT_EQ(args[0].data(), buf.data() + 8);
  EXPECT_EQ(args[1].data(), buf.data() + 17);
}

TEST(RespParse, ReplacesPreviousArguments) {
  std::vector<std::string_view> args = {"stale", "values", "here"};
  ASSERT_EQ(parse_command("*1\r\n$4\r\nPING\r\n", args).status, ParseStatus::Complete);
  EXPECT_EQ(args, (std::vector<std::string_view>{"PING"}));
}

// ---- Array format: errors ------------------------------------------------------------------

TEST(RespParse, InvalidArrayLength) {
  for (const char* buf : {"*x\r\n", "*\r\n", "*+1\r\n", "*-2\r\n", "*-0\r\n", "*01\r\n", "* 1\r\n",
                          "*1 \r\n", "*1x\r\n", "*1\n", "*99999999999999999999999999\r\n"}) {
    expect_error(buf);
  }
}

TEST(RespParse, ArrayLengthLimit) {
  const Limits limits{.max_array_len = 2};
  const std::string two = "*2\r\n$1\r\na\r\n$1\r\nb\r\n";
  const Parsed p = parse(two, limits);
  EXPECT_EQ(p.status, ParseStatus::Complete);
  // Rejected from the header alone, before any element arrives.
  expect_error("*3\r\n", limits);
}

TEST(RespParse, ElementMustBeABulkString) {
  expect_error("*1\r\n+GET\r\n");
  expect_error("*1\r\n:1\r\n");
  expect_error("*1\r\nGET\r\n");
}

TEST(RespParse, InvalidBulkLength) {
  for (const char* buf :
       {"*1\r\n$x\r\n", "*1\r\n$\r\n", "*1\r\n$-1\r\n", "*1\r\n$-2\r\n", "*1\r\n$+3\r\n",
        "*1\r\n$3 \r\n", "*1\r\n$03\r\n", "*1\r\n$3\n", "*1\r\n$99999999999999999999999999\r\n"}) {
    expect_error(buf);
  }
}

TEST(RespParse, BulkLengthLimit) {
  const Limits limits{.max_bulk_len = 4};
  const Parsed p = parse("*1\r\n$4\r\nPING\r\n", limits);
  EXPECT_EQ(p.status, ParseStatus::Complete);
  // Rejected from the header alone: the server must not wait for (and buffer) the payload.
  expect_error("*1\r\n$5\r\n", limits);
}

TEST(RespParse, BulkStringMustEndInCrlf) {
  expect_error("*1\r\n$3\r\nGETxx");
  expect_error("*1\r\n$3\r\nGET\rx");
  expect_error("*1\r\n$3\r\nGET\n\r");
  expect_need_more("*1\r\n$3\r\nGET\r");
}

TEST(RespParse, HeaderLineLengthLimit) {
  // A line must end within max_inline_len bytes, counting its '\n'. Every line below is 4 bytes;
  // the bulk payload is not a line.
  const Limits limits{.max_inline_len = 4};
  EXPECT_EQ(parse("*1\r\n$4\r\nPING\r\n", limits).status, ParseStatus::Complete);

  expect_need_more("*12\r", limits);     // 4 bytes, '\n' may still come
  expect_error("*123\r", limits);        // 5 bytes and no '\n'
  expect_error("*1\r\n$123\r", limits);  // the same for a bulk header
}

// ---- Inline format -----------------------------------------------------------------------

TEST(RespParse, InlineCommand) {
  expect_complete("PING\r\n", {"PING"});
  expect_complete("PING\n", {"PING"});
  expect_complete("SET k v\r\n", {"SET", "k", "v"});
}

TEST(RespParse, InlineSplitsOnRunsOfSpacesAndTabs) {
  expect_complete("  SET \t k   v \t\r\n", {"SET", "k", "v"});
}

TEST(RespParse, BlankInlineLineHasNoArguments) {
  expect_complete("\r\n", {});
  expect_complete("\n", {});
  expect_complete(" \t \r\n", {});
}

TEST(RespParse, InlineNeedsANewline) {
  expect_need_more("PING");
  expect_need_more("PING\r");
  expect_need_more("");
}

TEST(RespParse, InlineLineLengthLimit) {
  const Limits limits{.max_inline_len = 8};
  EXPECT_EQ(parse("GET abc\n", limits).status, ParseStatus::Complete);  // 8 bytes with '\n'
  expect_need_more("GET abcd", limits);                                 // '\n' could be next
  expect_error("GET abcde", limits);                                    // too late for '\n'
  expect_error("GET abcd\n", limits);                                   // '\n' is the 9th byte
}

// ---- Split reads ---------------------------------------------------------------------------

// Valid single commands in both formats, including empty ones and binary payloads, with the
// arguments each must parse to.
struct Case {
  std::string wire;
  Args args;
};

const std::vector<Case>& valid_commands() {
  static const std::vector<Case> cases = {
      {"*1\r\n$4\r\nPING\r\n", {"PING"}},
      {"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n", {"GET", "key"}},
      {"*3\r\n$3\r\nSET\r\n$10\r\nkey:000042\r\n$11\r\nab\r\n\0\r\n$*-1\r\n"s,
       {"SET", "key:000042", "ab\r\n\0\r\n$*-1"s}},
      {"*2\r\n$3\r\nGET\r\n$0\r\n\r\n", {"GET", ""}},
      {"*0\r\n", {}},
      {"*-1\r\n", {}},
      {"PING\r\n", {"PING"}},
      {"SET k v\n", {"SET", "k", "v"}},
      {"\r\n", {}},
  };
  return cases;
}

TEST(RespParse, EveryStrictPrefixNeedsMore) {
  // A connection may receive a command split at any byte; nothing before the last byte may be
  // reported as Complete or Error.
  for (const auto& [wire, args] : valid_commands()) {
    for (std::size_t n = 0; n < wire.size(); ++n) {
      expect_need_more(std::string_view(wire).substr(0, n));
    }
    expect_complete(wire, args);
  }
}

// Drives parse_command() the way a connection does: append each chunk to a buffer, parse every
// complete command, drop the consumed bytes, keep the partial tail.
std::vector<Args> parse_stream(const std::string& stream, std::size_t chunk) {
  std::vector<Args> out;
  std::string buf;
  std::vector<std::string_view> views;
  for (std::size_t pos = 0; pos < stream.size(); pos += chunk) {
    buf.append(stream, pos, chunk);
    std::size_t start = 0;
    for (;;) {
      const ParseResult r = parse_command(std::string_view(buf).substr(start), views);
      if (r.status != ParseStatus::Complete) {
        EXPECT_EQ(r.status, ParseStatus::NeedMore) << r.error;
        break;
      }
      out.emplace_back(views.begin(), views.end());
      start += r.consumed;
    }
    buf.erase(0, start);
  }
  EXPECT_TRUE(buf.empty()) << "unparsed tail: " << testing::PrintToString(buf);
  return out;
}

TEST(RespParse, PipelinedStreamInEveryChunkSize) {
  std::string stream;
  std::vector<Args> expected;
  for (const auto& [wire, args] : valid_commands()) {
    stream += wire;
    expected.push_back(args);
  }
  for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
    SCOPED_TRACE(testing::Message() << "chunk size " << chunk);
    EXPECT_EQ(parse_stream(stream, chunk), expected);
  }
}

// ---- Reply encoding ------------------------------------------------------------------------

TEST(RespEncode, SimpleString) {
  std::string out;
  append_simple_string(out, "OK");
  EXPECT_EQ(out, "+OK\r\n");
}

TEST(RespEncode, Error) {
  std::string out;
  append_error(out, "ERR unknown command 'FOO'");
  EXPECT_EQ(out, "-ERR unknown command 'FOO'\r\n");
}

TEST(RespEncode, Integer) {
  std::string out;
  append_integer(out, 0);
  append_integer(out, 42);
  append_integer(out, -1);
  append_integer(out, std::numeric_limits<std::int64_t>::min());
  append_integer(out, std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(out, ":0\r\n:42\r\n:-1\r\n:-9223372036854775808\r\n:9223372036854775807\r\n");
}

TEST(RespEncode, BulkString) {
  std::string out;
  append_bulk_string(out, "hello");
  append_bulk_string(out, "");
  append_bulk_string(out, "a\r\n\0"s);
  EXPECT_EQ(out, "$5\r\nhello\r\n$0\r\n\r\n$4\r\na\r\n\0\r\n"s);
}

TEST(RespEncode, NullBulkString) {
  std::string out;
  append_null_bulk_string(out);
  EXPECT_EQ(out, "$-1\r\n");
}

TEST(RespEncode, ArrayHeader) {
  std::string out;
  append_array_header(out, 0);
  append_array_header(out, 3);
  EXPECT_EQ(out, "*0\r\n*3\r\n");
}

TEST(RespEncode, AppendsWithoutClearing) {
  std::string out = "prefix";
  append_simple_string(out, "OK");
  EXPECT_EQ(out, "prefix+OK\r\n");
}

TEST(RespRoundTrip, EncodedArraysParseBack) {
  // Random binary arguments, encoded the way a client would, must parse back unchanged.
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<std::size_t> count(1, 6);
  std::uniform_int_distribution<std::size_t> len(0, 40);
  for (int iter = 0; iter < 200; ++iter) {
    Args args(count(rng));
    for (std::string& a : args) {
      a.resize(len(rng));
      for (char& c : a) c = static_cast<char>(byte(rng));
    }
    std::string wire;
    append_array_header(wire, args.size());
    for (const std::string& a : args) append_bulk_string(wire, a);
    expect_complete(wire, args);
  }
}

}  // namespace
}  // namespace lcache::resp
