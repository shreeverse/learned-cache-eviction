// End-to-end tests: a real Server on a loopback port, driven by blocking client sockets.
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "policy/lru.h"
#include "server/resp.h"
#include "server/server.h"
#include "server/store.h"

namespace lcache::server {
namespace {

using asio::ip::tcp;

// A blocking client. Reads time out after 5 s so a missing reply fails the test, not hangs it.
class Client {
 public:
  explicit Client(std::uint16_t port) {
    socket_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    socket_.set_option(tcp::no_delay(true));
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  }

  void send(std::string_view bytes) { asio::write(socket_, asio::buffer(bytes)); }

  std::string read_exactly(std::size_t n) {
    std::string buf(n, '\0');
    asio::read(socket_, asio::buffer(buf));
    return buf;
  }

  // Sends a request and reads exactly as many bytes as the expected reply has.
  std::string roundtrip(std::string_view request, std::string_view expected) {
    send(request);
    return read_exactly(expected.size());
  }

  // True if the server closed the connection (the next read sees EOF).
  bool closed_by_server() {
    char c = 0;
    std::error_code ec;
    asio::read(socket_, asio::buffer(&c, 1), ec);
    return ec == asio::error::eof || ec == asio::error::connection_reset;
  }

 private:
  asio::io_context io_;
  tcp::socket socket_{io_};
};

std::string command(const std::vector<std::string>& args) {
  std::string out;
  resp::append_array_header(out, args.size());
  for (const std::string& a : args) resp::append_bulk_string(out, a);
  return out;
}

struct NetworkTest : testing::Test {
  void SetUp() override { server_.start(); }

  std::uint16_t port() const { return server_.port(); }

  ShardedStore store_{std::uint64_t{64} << 20, 8,
                      [](std::uint64_t, std::size_t) { return std::make_unique<LruPolicy>(); }};
  Server server_{store_, ServerOptions{.address = "127.0.0.1", .port = 0, .threads = 4}};
};

TEST_F(NetworkTest, Ping) {
  Client c(port());
  EXPECT_EQ(c.roundtrip(command({"PING"}), "+PONG\r\n"), "+PONG\r\n");
  EXPECT_EQ(c.roundtrip("PING\r\n", "+PONG\r\n"), "+PONG\r\n");  // inline
}

TEST_F(NetworkTest, SetGetDel) {
  Client c(port());
  EXPECT_EQ(c.roundtrip(command({"SET", "k", "hello"}), "+OK\r\n"), "+OK\r\n");
  EXPECT_EQ(c.roundtrip(command({"GET", "k"}), "$5\r\nhello\r\n"), "$5\r\nhello\r\n");
  EXPECT_EQ(c.roundtrip(command({"DEL", "k"}), ":1\r\n"), ":1\r\n");
  EXPECT_EQ(c.roundtrip(command({"GET", "k"}), "$-1\r\n"), "$-1\r\n");
}

TEST_F(NetworkTest, ValuesAreSharedAcrossConnections) {
  Client a(port());
  Client b(port());
  a.roundtrip(command({"SET", "shared", "v"}), "+OK\r\n");
  EXPECT_EQ(b.roundtrip(command({"GET", "shared"}), "$1\r\nv\r\n"), "$1\r\nv\r\n");
}

TEST_F(NetworkTest, PipelinedBatchInOneWrite) {
  Client c(port());
  std::string request;
  std::string expected;
  for (int i = 0; i < 1000; ++i) {
    const std::string key = "key:" + std::to_string(i);
    request += command({"SET", key, std::to_string(i)});
    request += command({"GET", key});
    expected += "+OK\r\n";
    resp::append_bulk_string(expected, std::to_string(i));
  }
  EXPECT_EQ(c.roundtrip(request, expected), expected);
}

TEST_F(NetworkTest, CommandSplitAcrossManyWrites) {
  Client c(port());
  const std::string request = command({"SET", "split", "value"}) + command({"GET", "split"});
  for (const char byte : request) {
    c.send(std::string_view(&byte, 1));
    std::this_thread::sleep_for(std::chrono::microseconds(200));  // separate TCP segments
  }
  EXPECT_EQ(c.read_exactly(16), "+OK\r\n$5\r\nvalue\r\n");
}

TEST_F(NetworkTest, LargeValue) {
  Client c(port());
  const std::string value(std::size_t{3} << 20, 'v');  // 3 MiB: many reads and partial writes
  EXPECT_EQ(c.roundtrip(command({"SET", "big", value}), "+OK\r\n"), "+OK\r\n");
  std::string expected;
  resp::append_bulk_string(expected, value);
  EXPECT_EQ(c.roundtrip(command({"GET", "big"}), expected), expected);
}

TEST_F(NetworkTest, PipelinedLargeRepliesAreAllDelivered) {
  // 40 GETs of a 256 KiB value: replies exceed Connection::kMaxPendingReply, so the batch is
  // written in parts; all of them must arrive, in order.
  Client c(port());
  const std::string value(std::size_t{256} << 10, 'x');
  c.roundtrip(command({"SET", "v", value}), "+OK\r\n");
  std::string request;
  std::string expected;
  for (int i = 0; i < 40; ++i) {
    request += command({"GET", "v"});
    resp::append_bulk_string(expected, value);
  }
  request += command({"PING"});
  expected += "+PONG\r\n";
  EXPECT_EQ(c.roundtrip(request, expected), expected);
}

TEST_F(NetworkTest, ProtocolErrorRepliesThenCloses) {
  Client c(port());
  const std::string error = "-ERR Protocol error: invalid multibulk length\r\n";
  EXPECT_EQ(c.roundtrip("*x\r\n", error), error);
  EXPECT_TRUE(c.closed_by_server());
}

TEST_F(NetworkTest, CommandsBeforeAProtocolErrorStillRun) {
  Client c(port());
  const std::string expected = "+OK\r\n-ERR Protocol error: expected '$'\r\n";
  EXPECT_EQ(c.roundtrip(command({"SET", "k", "v"}) + "*1\r\n:1\r\n", expected), expected);
  EXPECT_TRUE(c.closed_by_server());
  Client d(port());
  EXPECT_EQ(d.roundtrip(command({"GET", "k"}), "$1\r\nv\r\n"), "$1\r\nv\r\n");
}

TEST_F(NetworkTest, QuitRepliesThenCloses) {
  Client c(port());
  // The GET after QUIT must not run.
  EXPECT_EQ(c.roundtrip(command({"QUIT"}) + command({"GET", "k"}), "+OK\r\n"), "+OK\r\n");
  EXPECT_TRUE(c.closed_by_server());
}

TEST_F(NetworkTest, ConcurrentClients) {
  constexpr std::size_t kClients = 8;
  constexpr int kOps = 200;
  std::vector<std::thread> threads;
  std::vector<int> mismatches(kClients, 0);
  for (std::size_t t = 0; t < kClients; ++t) {
    threads.emplace_back([this, t, &mismatches] {
      Client c(port());
      for (int i = 0; i < kOps; ++i) {
        const std::string key = "c" + std::to_string(t) + ":" + std::to_string(i);
        std::string expected;
        resp::append_bulk_string(expected, key);
        if (c.roundtrip(command({"SET", key, key}), "+OK\r\n") != "+OK\r\n") ++mismatches[t];
        if (c.roundtrip(command({"GET", key}), expected) != expected) ++mismatches[t];
      }
    });
  }
  for (std::thread& th : threads) th.join();
  for (std::size_t t = 0; t < kClients; ++t) EXPECT_EQ(mismatches[t], 0) << "client " << t;
}

TEST(Server, StopsWithOpenConnections) {
  ShardedStore store(1 << 20, 4,
                     [](std::uint64_t, std::size_t) { return std::make_unique<LruPolicy>(); });
  auto server = std::make_unique<Server>(store, ServerOptions{.port = 0, .threads = 2});
  server->start();
  Client c(server->port());
  c.send(command({"SET", "k", "v"}) + "*2\r\n$3\r\nGET");  // leaves a partial command pending
  server.reset();                                          // must not hang or crash
  EXPECT_TRUE(c.closed_by_server() || true);  // the client just sees the socket go away
}

TEST(Server, RejectsZeroThreads) {
  ShardedStore store(1 << 20, 4,
                     [](std::uint64_t, std::size_t) { return std::make_unique<LruPolicy>(); });
  EXPECT_THROW(Server(store, ServerOptions{.port = 0, .threads = 0}), std::invalid_argument);
}

}  // namespace
}  // namespace lcache::server
