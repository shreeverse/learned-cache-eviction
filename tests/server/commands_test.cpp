#include "server/commands.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "policy/lru.h"

namespace lcache::server {
namespace {

struct CommandsTest : testing::Test {
  // Runs one command and returns its reply.
  std::string run(std::vector<std::string_view> args) {
    std::string out;
    last_after = execute(args, store, out);
    return out;
  }

  ShardedStore store{1024, 2,
                     [](std::uint64_t, std::size_t) { return std::make_unique<LruPolicy>(); }};
  After last_after = After::Continue;
};

TEST_F(CommandsTest, SetGetDel) {
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
  EXPECT_EQ(run({"SET", "k", "hello"}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$5\r\nhello\r\n");
  EXPECT_EQ(run({"DEL", "k", "missing", "k"}), ":1\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
  EXPECT_EQ(last_after, After::Continue);
}

TEST_F(CommandsTest, NamesAreCaseInsensitive) {
  EXPECT_EQ(run({"set", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(run({"gEt", "k"}), "$1\r\nv\r\n");
}

TEST_F(CommandsTest, BinaryValues) {
  const std::string v("a\r\n\0b", 5);
  EXPECT_EQ(run({"SET", "k", v}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$5\r\n" + v + "\r\n");
}

TEST_F(CommandsTest, DelCountsEachRemovedKey) {
  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"DEL", "a", "b", "c"}), ":2\r\n");
}

TEST_F(CommandsTest, WrongArity) {
  EXPECT_EQ(run({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
  EXPECT_EQ(run({"GET", "a", "b"}), "-ERR wrong number of arguments for 'get' command\r\n");
  EXPECT_EQ(run({"set", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
  EXPECT_EQ(run({"DEL"}), "-ERR wrong number of arguments for 'del' command\r\n");
  EXPECT_EQ(run({"PING", "a", "b"}), "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST_F(CommandsTest, SetOptionsAreASyntaxError) {
  EXPECT_EQ(run({"SET", "k", "v", "EX", "10"}), "-ERR syntax error\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandsTest, TooLargeValue) {
  // 1024 bytes over 2 shards: 512 per shard.
  EXPECT_EQ(run({"SET", "k", std::string(600, 'x')}),
            "-ERR key and value are larger than a cache shard\r\n");
}

TEST_F(CommandsTest, Ping) {
  EXPECT_EQ(run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(run({"ping", "hi"}), "$2\r\nhi\r\n");
}

TEST_F(CommandsTest, QuitClosesAfterReplying) {
  EXPECT_EQ(run({"QUIT"}), "+OK\r\n");
  EXPECT_EQ(last_after, After::Close);
}

TEST_F(CommandsTest, CommandRepliesWithAnEmptyArray) {
  EXPECT_EQ(run({"COMMAND"}), "*0\r\n");
  EXPECT_EQ(run({"COMMAND", "DOCS"}), "*0\r\n");
}

TEST_F(CommandsTest, ConfigGetAnswersWhatRedisBenchmarkAsks) {
  EXPECT_EQ(run({"CONFIG", "GET", "save"}), "*2\r\n$4\r\nsave\r\n$0\r\n\r\n");
  EXPECT_EQ(run({"config", "get", "appendonly"}), "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n");
  EXPECT_EQ(run({"CONFIG", "GET", "maxmemory"}), "*0\r\n");
  EXPECT_EQ(run({"CONFIG", "SET", "save", ""}), "-ERR only CONFIG GET is supported\r\n");
  EXPECT_EQ(run({"CONFIG", "GET"}), "-ERR wrong number of arguments for 'config|get' command\r\n");
}

TEST_F(CommandsTest, UnknownCommandIsEchoedSafely) {
  EXPECT_EQ(run({"FLUSHALL"}), "-ERR unknown command 'FLUSHALL'\r\n");
  // A CR/LF in the name must not end the error line early.
  EXPECT_EQ(run({"a\r\nb"}), "-ERR unknown command 'a  b'\r\n");
  const std::string reply = run({std::string(1000, 'x')});
  EXPECT_EQ(reply.size(), std::string("-ERR unknown command ''\r\n").size() + 128);
}

}  // namespace
}  // namespace lcache::server
