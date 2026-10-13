// lcache-api: JSON HTTP API over the simulator, for the web dashboard (docs/m6-design.md).
//
//   lcache-api --data-dir data --model models/twitter26_1MiB_h10k_32x31.txt --port 8080
//
// Routes: GET /api/traces, GET /api/policies, POST /api/simulate. Errors are {"error": "..."}.
// All logic is in lc_api (api/api.h); this file only maps routes and exceptions to HTTP.

#include <httplib.h>
#include <pthread.h>

#include <CLI/CLI.hpp>
#include <csignal>
#include <exception>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "api/api.h"

namespace {

using nlohmann::json;

void send_json(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

int run(int argc, char** argv) {
  CLI::App app{"JSON HTTP API over the cache simulator"};
  std::string host = "127.0.0.1";
  int port = 8080;
  lcache::api::ApiOptions options;
  options.model_path = "models/twitter26_1MiB_h10k_32x31.txt";
  std::string data_dir = options.data_dir.string();

  app.add_option("--host", host, "Address to listen on")->capture_default_str();
  app.add_option("-p,--port", port, "Port to listen on")->capture_default_str();
  app.add_option("-d,--data-dir", data_dir, "Directory with the trace files offered")
      ->capture_default_str()
      ->check(CLI::ExistingDirectory);
  app.add_option("--model", options.model_path,
                 "Model for 'learned' and 'guarded' (empty or missing: they are unavailable)")
      ->capture_default_str();
  app.add_option("-j,--workers", options.workers, "Runs simulated in parallel per request")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);
  options.data_dir = data_dir;

  // Block SIGINT / SIGTERM in every thread (httplib's workers inherit this mask); the main
  // thread waits for them with sigwait() and stops the server cleanly.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  const lcache::api::Api api(options);
  if (!api.model_available()) {
    std::cerr << "lcache-api: 'learned' and 'guarded' are unavailable: " << api.model_error()
              << '\n';
  }

  httplib::Server server;
  server.set_payload_max_length(std::size_t{64} *
                                1024);  // a simulate request is a few hundred bytes

  server.Get("/api/traces", [&](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, api.traces());
  });
  server.Get("/api/policies", [&](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, api.policies());
  });

  // One simulation at a time (decisions D26): bounded memory and CPU; a second request gets 429.
  std::mutex simulate_mutex;
  server.Post("/api/simulate", [&](const httplib::Request& req, httplib::Response& res) {
    const std::unique_lock lock(simulate_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      send_json(res, 429, {{"error", "a simulation is already running; try again shortly"}});
      return;
    }
    const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
      send_json(res, 400, {{"error", "body is not valid JSON"}});
      return;
    }
    send_json(res, 200, api.simulate(body));
  });

  server.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, const std::exception_ptr& ep) {
        try {
          std::rethrow_exception(ep);
        } catch (const lcache::api::ApiError& e) {
          send_json(res, e.status(), {{"error", e.what()}});
        } catch (const std::exception& e) {
          send_json(res, 500, {{"error", std::string("internal error: ") + e.what()}});
        } catch (...) {
          send_json(res, 500, {{"error", "internal error"}});
        }
      });
  // httplib calls this for every status >= 400; only fill in a body where there is none
  // (unknown route, payload too large, ...).
  server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    if (res.body.empty()) {
      send_json(res, res.status,
                {{"error", std::string(httplib::status_message(res.status)) + ": " + req.path}});
    }
  });
  server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
    std::cerr << req.method << ' ' << req.path << ' ' << res.status << '\n';
  });

  if (!server.bind_to_port(host, port)) {
    std::cerr << "lcache-api: cannot listen on " << host << ':' << port << '\n';
    return 1;
  }
  std::thread listener([&] { server.listen_after_bind(); });
  std::cout << "lcache-api listening on http://" << host << ':' << port
            << " (data: " << options.data_dir.string() << ", workers: " << options.workers << ")"
            << std::endl;

  int sig = 0;
  sigwait(&signals, &sig);
  server.stop();
  listener.join();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "lcache-api: " << e.what() << '\n';
    return 1;
  }
}
