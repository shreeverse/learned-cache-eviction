#pragma once

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "server/resp.h"
#include "server/store.h"

namespace lcache::server {

struct ServerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 6380;  // 0 = any free port (see Server::port())
  std::size_t threads = 4;    // I/O threads, one io_context each
  resp::Limits limits{};
};

// Accepts connections and serves them on N threads, each running its own io_context; a
// connection is assigned round-robin and stays on its thread (decisions D22). Any thread may
// use any shard of the store, which locks per shard.
class Server {
 public:
  // Binds and listens right away, so port() is valid and a busy port fails here (asio throws
  // std::system_error). Throws std::invalid_argument if threads == 0. `store` must outlive this.
  Server(ShardedStore& store, ServerOptions options);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;

  void start();  // starts the I/O threads; call once
  // Stops accepting, abandons open connections and joins the threads. Idempotent; called by
  // the destructor. Must not be called from an I/O thread.
  void stop();

  std::uint16_t port() const { return port_; }

 private:
  void accept();

  ShardedStore& store_;
  ServerOptions options_;
  std::vector<std::unique_ptr<asio::io_context>> contexts_;
  std::vector<asio::executor_work_guard<asio::io_context::executor_type>> work_;
  asio::ip::tcp::acceptor acceptor_;  // runs on contexts_[0]; destroyed before contexts_
  std::uint16_t port_ = 0;
  std::size_t next_context_ = 0;  // used only by accept handlers (contexts_[0]'s thread)
  std::vector<std::thread> threads_;
};

}  // namespace lcache::server
