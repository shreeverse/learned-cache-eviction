#include "server/commands.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>

#include "server/resp.h"

namespace lcache::server {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

// A client-supplied name, made safe to echo in an error line: at most 128 bytes, and no CR/LF
// (which would end the reply early) or other control bytes.
std::string printable(std::string_view s) {
  std::string out(s.substr(0, 128));
  for (char& c : out) {
    if (std::iscntrl(static_cast<unsigned char>(c)) != 0) c = ' ';
  }
  return out;
}

void wrong_arity(std::string& out, std::string_view name) {
  std::string lower = printable(name);
  std::ranges::transform(lower, lower.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  resp::append_error(out, "ERR wrong number of arguments for '" + lower + "' command");
}

void get(std::span<const std::string_view> args, ShardedStore& store, std::string& out) {
  if (args.size() != 2) return wrong_arity(out, args[0]);
  // Encoded under the shard lock: the value is copied once, straight into the reply.
  const bool hit =
      store.get(args[1], [&out](std::string_view v) { resp::append_bulk_string(out, v); });
  if (!hit) resp::append_null_bulk_string(out);
}

void set(std::span<const std::string_view> args, ShardedStore& store, std::string& out) {
  if (args.size() < 3) return wrong_arity(out, args[0]);
  if (args.size() > 3) return resp::append_error(out, "ERR syntax error");
  switch (store.set(args[1], args[2])) {
    case SetResult::Stored:
      return resp::append_simple_string(out, "OK");
    case SetResult::TooLarge:
      return resp::append_error(out, "ERR key and value are larger than a cache shard");
  }
}

void del(std::span<const std::string_view> args, ShardedStore& store, std::string& out) {
  if (args.size() < 2) return wrong_arity(out, args[0]);
  std::int64_t removed = 0;
  for (const std::string_view key : args.subspan(1)) removed += store.del(key) ? 1 : 0;
  resp::append_integer(out, removed);
}

void ping(std::span<const std::string_view> args, std::string& out) {
  if (args.size() == 1) return resp::append_simple_string(out, "PONG");
  if (args.size() == 2) return resp::append_bulk_string(out, args[1]);
  wrong_arity(out, args[0]);
}

void config(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2 || !iequals(args[1], "GET")) {
    return resp::append_error(out, "ERR only CONFIG GET is supported");
  }
  if (args.size() < 3) return wrong_arity(out, "config|get");
  // Persistence settings, as a cache without persistence would report them.
  std::size_t found = 0;
  std::string pairs;
  for (const std::string_view name : args.subspan(2)) {
    if (iequals(name, "save")) {
      resp::append_bulk_string(pairs, "save");
      resp::append_bulk_string(pairs, "");
      ++found;
    } else if (iequals(name, "appendonly")) {
      resp::append_bulk_string(pairs, "appendonly");
      resp::append_bulk_string(pairs, "no");
      ++found;
    }
  }
  resp::append_array_header(out, 2 * found);
  out += pairs;
}

}  // namespace

After execute(std::span<const std::string_view> args, ShardedStore& store, std::string& out) {
  const std::string_view name = args[0];
  if (iequals(name, "GET")) {
    get(args, store, out);
  } else if (iequals(name, "SET")) {
    set(args, store, out);
  } else if (iequals(name, "DEL")) {
    del(args, store, out);
  } else if (iequals(name, "PING")) {
    ping(args, out);
  } else if (iequals(name, "QUIT")) {
    resp::append_simple_string(out, "OK");
    return After::Close;
  } else if (iequals(name, "COMMAND")) {
    resp::append_array_header(out, 0);
  } else if (iequals(name, "CONFIG")) {
    config(args, out);
  } else {
    resp::append_error(out, "ERR unknown command '" + printable(name) + "'");
  }
  return After::Continue;
}

}  // namespace lcache::server
