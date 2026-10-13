// lcache-api: JSON HTTP API over the simulator, for the web dashboard (docs/m6-design.md).
//
//   lcache-api --data-dir data --model models/twitter26_1MiB_h10k_32x31.txt --port 8080
//
// Routes are in api/routes.h; all logic is in lc_api (api/api.h). This file handles options,
// signals and startup.

#include <httplib.h>
#include <pthread.h>

#include <CLI/CLI.hpp>
#include <csignal>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "api/api.h"
#include "api/routes.h"

namespace {

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
  std::mutex busy;  // one simulation or window at a time (decisions D26)
  lcache::api::register_routes(server, api, busy, &std::cerr);

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
