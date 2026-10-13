#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "server/store.h"

namespace lcache::server {

enum class After : std::uint8_t {
  Continue,
  Close,  // QUIT: send the reply, then close the connection
};

// Executes one parsed command against the store and appends its RESP reply to `out`.
// `args` is non-empty; command names are case-insensitive. Supported (docs/m5-design.md):
//   GET key             -> bulk string, or null on a miss
//   SET key value       -> +OK (no options: EX, NX, ... reply "syntax error")
//   DEL key [key ...]   -> :number of keys removed
//   PING [message]      -> +PONG, or the message as a bulk string
//   QUIT                -> +OK, then After::Close
//   COMMAND [...]       -> empty array (redis-cli asks for command docs on connect)
//   CONFIG GET name ... -> name/value pairs for "save" and "appendonly" (what redis-benchmark
//                          reads at startup), nothing for other names
// Anything else gets an error reply in Redis's wording.
After execute(std::span<const std::string_view> args, ShardedStore& store, std::string& out);

}  // namespace lcache::server
