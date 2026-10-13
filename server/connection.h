#pragma once

#include <asio/ip/tcp.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "server/resp.h"
#include "server/store.h"

namespace lcache::server {

// One client connection. All its handlers run on the io_context that owns its socket (one
// thread), so it needs no locking of its own (decisions D22); only the store is shared.
//
// The loop alternates strictly between reading and writing, so at most one async operation is
// outstanding and a client that doesn't read its replies stops being read (backpressure):
//   read some bytes -> parse and execute every complete command (pipelining), appending the
//   replies to one buffer -> write the buffer with a single async_write -> repeat.
// A batch stops early once the replies exceed kMaxPendingReply, so pipelined GETs of large
// values can't make the reply buffer grow without bound; the remaining commands run after the
// write. A protocol error sends the error reply and closes, as does QUIT.
//
// Lifetime: owned by the shared_ptrs in its pending handlers; it is destroyed when the
// connection closes, or when the io_context is destroyed with handlers still pending.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  static constexpr std::size_t kReadChunk = std::size_t{16} << 10;
  static constexpr std::size_t kMaxPendingReply = std::size_t{1} << 20;

  Connection(asio::ip::tcp::socket socket, ShardedStore& store, const resp::Limits& limits);

  void start();

 private:
  void advance();  // run buffered commands, then write, read or close
  void process();
  void read();
  void write();
  void close();

  asio::ip::tcp::socket socket_;
  ShardedStore& store_;
  resp::Limits limits_;

  std::string in_;  // bytes received; the first in_used_ are valid
  std::size_t in_used_ = 0;
  std::string out_;                     // replies not yet written
  std::vector<std::string_view> args_;  // reused across commands
  bool close_after_write_ = false;
};

}  // namespace lcache::server
