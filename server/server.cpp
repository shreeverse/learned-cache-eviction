#include "server/server.h"

#include <asio/ip/address.hpp>
#include <asio/post.hpp>
#include <stdexcept>
#include <utility>

#include "server/connection.h"

namespace lcache::server {

namespace {

std::vector<std::unique_ptr<asio::io_context>> make_contexts(std::size_t n) {
  if (n == 0) throw std::invalid_argument("Server: needs at least one thread");
  std::vector<std::unique_ptr<asio::io_context>> contexts;
  contexts.reserve(n);
  // Concurrency hint 1: each io_context is run by exactly one thread, so Asio can skip its
  // internal locking.
  for (std::size_t i = 0; i < n; ++i) contexts.push_back(std::make_unique<asio::io_context>(1));
  return contexts;
}

}  // namespace

Server::Server(ShardedStore& store, ServerOptions options)
    : store_(store),
      options_(std::move(options)),
      contexts_(make_contexts(options_.threads)),
      acceptor_(*contexts_[0]) {
  const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(options_.address), options_.port);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
  port_ = acceptor_.local_endpoint().port();
}

Server::~Server() { stop(); }

void Server::start() {
  accept();
  for (const auto& ctx : contexts_) work_.push_back(asio::make_work_guard(*ctx));
  threads_.reserve(contexts_.size());
  for (const auto& ctx : contexts_) {
    threads_.emplace_back([&io = *ctx] { io.run(); });
  }
}

void Server::accept() {
  // The new socket belongs to the next io_context, so its connection runs on that thread.
  asio::io_context& target = *contexts_[next_context_];
  next_context_ = (next_context_ + 1) % contexts_.size();
  acceptor_.async_accept(target, [this](std::error_code ec, asio::ip::tcp::socket socket) {
    if (ec == asio::error::operation_aborted) return;  // acceptor closed by stop()
    if (!ec) {
      // This handler runs on contexts_[0]'s thread; start the connection on its own thread so
      // that only that thread ever touches it.
      auto executor = socket.get_executor();
      auto conn = std::make_shared<Connection>(std::move(socket), store_, options_.limits);
      asio::post(executor, [conn = std::move(conn)] { conn->start(); });
    }
    // Other errors (e.g. out of file descriptors) affect one connection; keep accepting.
    accept();
  });
}

void Server::stop() {
  if (threads_.empty()) return;
  // stop() is thread-safe; pending handlers (and the connections they own) are destroyed with
  // the io_contexts, after the threads have finished.
  for (const auto& ctx : contexts_) ctx->stop();
  for (std::thread& t : threads_) t.join();
  threads_.clear();
  work_.clear();
  std::error_code ignored;
  acceptor_.close(ignored);  // no thread runs contexts_[0] any more
}

}  // namespace lcache::server
