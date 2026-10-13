// lcache-replay: replays a trace against lcache-server over RESP with cache-aside semantics
// (GET; on a miss, SET), and reports the hit rate the server achieved (docs/m5-design.md
// section 6).
//
// Requests map to commands so the server charges exactly what the simulator charges: the key is
// the object id in decimal and the value is (size - key length) bytes, so key + value = size.
// Objects smaller than their key can't be represented and are counted (none in the Twitter
// trace).
//
// --mode simulate runs the simulator core (sim::Cache) in-process instead, on the same requests
// with each object id replaced by the server's hash of its key. Policies then see the same ids
// as in the server, so a 1-shard server must match it exactly for every deterministic policy.
// (cachesim, with the original ids, matches exactly only for policies that ignore id values,
// such as LRU; W-TinyLFU's sketch, for one, hashes them.)
#include <CLI/CLI.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "apps/cachesim/size_parse.h"
#include "ml/model_meta.h"
#include "policy/policy_factory.h"
#include "server/resp.h"
#include "server/store.h"
#include "sim/cache.h"
#include "trace/reader_factory.h"

namespace {

using asio::ip::tcp;

// A blocking RESP client that reads replies through a buffer.
class RespClient {
 public:
  RespClient(const std::string& host, std::uint16_t port) {
    tcp::resolver resolver(io_);
    asio::connect(socket_, resolver.resolve(host, std::to_string(port)));
    socket_.set_option(tcp::no_delay(true));
  }

  void send(std::string_view bytes) { asio::write(socket_, asio::buffer(bytes)); }

  // Reads one reply line without its "\r\n".
  std::string_view read_line() {
    for (;;) {
      const std::size_t eol = std::string_view(buf_).find("\r\n", pos_);
      if (eol != std::string_view::npos) {
        const std::string_view line(buf_.data() + pos_, eol - pos_);
        pos_ = eol + 2;
        return line;
      }
      fill();
    }
  }

  // Skips n bytes of payload plus its "\r\n".
  void skip(std::size_t n) {
    n += 2;
    while (buf_.size() - pos_ < n) {
      n -= buf_.size() - pos_;
      pos_ = buf_.size();
      fill();
    }
    pos_ += n;
  }

 private:
  void fill() {
    buf_.erase(0, pos_);
    pos_ = 0;
    const std::size_t old = buf_.size();
    buf_.resize(old + kChunk);
    const std::size_t n = socket_.read_some(asio::buffer(buf_.data() + old, kChunk));
    buf_.resize(old + n);
  }

  static constexpr std::size_t kChunk = std::size_t{64} << 10;

  asio::io_context io_;
  tcp::socket socket_{io_};
  std::string buf_;
  std::size_t pos_ = 0;
};

struct Counts {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_hit = 0;
  std::uint64_t too_large = 0;    // SETs the server refused (bigger than a shard)
  std::uint64_t unencodable = 0;  // objects smaller than their decimal key
};

// The value for an object of `size` bytes whose key has key_len bytes.
std::string_view value_for(std::uint32_t size, std::size_t key_len, const std::string& pool) {
  return std::string_view(pool).substr(0, size - key_len);
}

Counts replay_network(lcache::TraceReader& reader, std::uint64_t max_requests,
                      const std::string& host, std::uint16_t port) {
  RespClient client(host, port);
  const std::string pool(std::size_t{1} << 26, 'v');  // 64 MiB of value bytes, sliced per SET
  Counts c;
  std::string cmd;
  lcache::Request r;
  while ((max_requests == 0 || c.requests < max_requests) && reader.next(r)) {
    const std::string key = std::to_string(r.key);
    ++c.requests;
    c.bytes_requested += r.size;

    cmd.clear();
    lcache::resp::append_array_header(cmd, 2);
    lcache::resp::append_bulk_string(cmd, "GET");
    lcache::resp::append_bulk_string(cmd, key);
    client.send(cmd);
    const std::string_view reply = client.read_line();
    if (reply.empty() || reply[0] != '$') throw std::runtime_error("GET: unexpected reply");
    if (reply != "$-1") {
      ++c.hits;
      c.bytes_hit += r.size;
      client.skip(std::stoull(std::string(reply.substr(1))));
      continue;
    }

    if (r.size < key.size() || r.size - key.size() > pool.size()) {
      ++c.unencodable;
      continue;
    }
    cmd.clear();
    lcache::resp::append_array_header(cmd, 3);
    lcache::resp::append_bulk_string(cmd, "SET");
    lcache::resp::append_bulk_string(cmd, key);
    lcache::resp::append_bulk_string(cmd, value_for(r.size, key.size(), pool));
    client.send(cmd);
    const std::string_view set_reply = client.read_line();
    if (set_reply.starts_with("-ERR key and value are larger")) {
      ++c.too_large;
    } else if (set_reply != "+OK") {
      throw std::runtime_error("SET: unexpected reply: " + std::string(set_reply));
    }
  }
  return c;
}

Counts replay_simulate(lcache::TraceReader& reader, std::uint64_t max_requests,
                       lcache::EvictionPolicy& policy, std::uint64_t capacity) {
  lcache::Cache cache(policy, capacity);
  Counts c;
  lcache::Request r;
  while ((max_requests == 0 || c.requests < max_requests) && reader.next(r)) {
    const std::string key = std::to_string(r.key);
    ++c.requests;
    c.bytes_requested += r.size;
    if (r.size < key.size()) {
      ++c.unencodable;
      continue;
    }
    if (r.size > capacity) ++c.too_large;  // never inserted, like a refused SET
    r.key = lcache::server::default_key_hash(key);
    if (cache.access(r)) {
      ++c.hits;
      c.bytes_hit += r.size;
    }
  }
  return c;
}

}  // namespace

int main(int argc, char** argv) try {
  CLI::App app{"lcache-replay: replay a trace against lcache-server (GET, SET on miss)"};
  std::string trace_path;
  std::string format = "auto";
  std::uint64_t max_requests = 0;
  std::string mode = "network";
  std::string host = "127.0.0.1";
  std::uint16_t port = 6380;
  std::string policy = "lru";
  std::string capacity_arg;
  std::size_t shards = 1;
  lcache::PolicyConfig config;

  app.add_option("trace", trace_path, "Trace file")->required()->check(CLI::ExistingFile);
  app.add_option("-f,--format", format, "oracle_general | csv | auto")->capture_default_str();
  app.add_option("-n,--max-requests", max_requests, "Stop after N requests (0 = all)")
      ->capture_default_str();
  app.add_option("--mode", mode,
                 "network: replay against a running lcache-server; simulate: sim::Cache "
                 "in-process with the server's key hashing (the 1-shard reference)")
      ->check(CLI::IsMember({"network", "simulate"}))
      ->capture_default_str();
  app.add_option("--host", host, "network: server address")->capture_default_str();
  app.add_option("-p,--port", port, "network: server port")->capture_default_str();
  app.add_option("--policy", policy,
                 "simulate: the policy; network: only a label (the server's --policy decides)")
      ->capture_default_str();
  app.add_option("-c,--capacity", capacity_arg,
                 "simulate: cache capacity; network: a label (bytes or KiB/MiB/...)")
      ->required();
  app.add_option("--shards", shards, "network: a label (the server's --shards decides)")
      ->capture_default_str();
  app.add_option("--model", config.model_path, "simulate: model for learned / guarded")
      ->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);
  const std::uint64_t capacity = lcache::parse_size(capacity_arg);

  auto reader = lcache::make_reader(format, trace_path);
  const auto start = std::chrono::steady_clock::now();
  Counts c;
  if (mode == "network") {
    c = replay_network(*reader, max_requests, host, port);
  } else {
    if (shards != 1) throw std::invalid_argument("--mode simulate models one shard");
    if (!config.model_path.empty()) {
      const auto f = lcache::resolve_model_features(config.model_path, {}, false, false);
      config.history = f.history;
      config.history_ratio = f.history_ratio;
    }
    config.capacity_bytes = capacity;
    const auto p = lcache::make_policy(policy, config);
    c = replay_simulate(*reader, max_requests, *p, capacity);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const auto ratio = [](std::uint64_t a, std::uint64_t b) {
    return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
  };
  std::cout << "mode,policy,capacity,shards,requests,hits,hit_rate,byte_hit_rate,too_large,"
               "unencodable,seconds\n"
            << mode << ',' << policy << ',' << capacity << ',' << shards << ',' << c.requests << ','
            << c.hits << ',' << ratio(c.hits, c.requests) << ','
            << ratio(c.bytes_hit, c.bytes_requested) << ',' << c.too_large << ',' << c.unencodable
            << ',' << seconds << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "lcache-replay: " << e.what() << '\n';
  return 1;
}
