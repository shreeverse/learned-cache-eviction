#include "server/connection.h"

#include <algorithm>
#include <asio/buffer.hpp>
#include <asio/write.hpp>
#include <utility>

#include "server/commands.h"

namespace lcache::server {

Connection::Connection(asio::ip::tcp::socket socket, ShardedStore& store,
                       const resp::Limits& limits)
    : socket_(std::move(socket)), store_(store), limits_(limits) {}

void Connection::start() {
  std::error_code ignored;
  socket_.set_option(asio::ip::tcp::no_delay(true), ignored);  // replies are small
  read();
}

// advance -> read/write -> completion handler -> advance looks recursive to clang-tidy, but Asio
// never runs a completion handler inside the call that starts the operation (it is queued on the
// io_context), so the stack unwinds between steps.
void Connection::advance() {  // NOLINT(misc-no-recursion)
  process();
  if (!out_.empty()) {
    write();
  } else if (close_after_write_) {
    close();
  } else {
    read();
  }
}

void Connection::process() {
  std::size_t offset = 0;
  while (!close_after_write_ && out_.size() < kMaxPendingReply) {
    const std::string_view pending(in_.data() + offset, in_used_ - offset);
    const resp::ParseResult r = resp::parse_command(pending, args_, limits_);
    if (r.status == resp::ParseStatus::NeedMore) break;
    if (r.status == resp::ParseStatus::Error) {
      resp::append_error(out_, r.error);
      close_after_write_ = true;
      break;
    }
    offset += r.consumed;
    if (!args_.empty() && execute(args_, store_, out_) == After::Close) close_after_write_ = true;
  }
  // Keep the unparsed tail at the front of the buffer.
  std::copy(in_.begin() + static_cast<std::ptrdiff_t>(offset),
            in_.begin() + static_cast<std::ptrdiff_t>(in_used_), in_.begin());
  in_used_ -= offset;
  // Don't keep a large buffer (after a big value) for an idle connection.
  if (in_used_ == 0 && in_.size() > 4 * kReadChunk) std::string().swap(in_);
}

void Connection::read() {  // NOLINT(misc-no-recursion): see advance()
  if (in_.size() - in_used_ < kReadChunk) in_.resize(in_used_ + kReadChunk);
  socket_.async_read_some(asio::buffer(in_.data() + in_used_, in_.size() - in_used_),
                          [self = shared_from_this()](std::error_code ec, std::size_t n) {
                            if (ec) return self->close();  // EOF, reset, or shutting down
                            self->in_used_ += n;
                            self->advance();
                          });
}

void Connection::write() {  // NOLINT(misc-no-recursion): see advance()
  asio::async_write(socket_, asio::buffer(out_),
                    // NOLINTNEXTLINE(misc-no-recursion): see advance()
                    [self = shared_from_this()](std::error_code ec, std::size_t /*n*/) {
                      if (ec) return self->close();
                      self->out_.clear();
                      self->advance();
                    });
}

void Connection::close() {
  std::error_code ignored;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

}  // namespace lcache::server
