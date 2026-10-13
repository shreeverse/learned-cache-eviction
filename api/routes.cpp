#include "api/routes.h"

#include <cstddef>
#include <exception>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace lcache::api {

namespace {

using nlohmann::json;

void send_json(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

// A POST route that parses the JSON body and runs `handler` while holding `busy`.
httplib::Server::Handler heavy_post(std::mutex& busy, std::function<json(const json&)> handler) {
  return
      [&busy, handler = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
        const std::unique_lock lock(busy, std::try_to_lock);
        if (!lock.owns_lock()) {
          send_json(res, 429, {{"error", "a simulation is already running; try again shortly"}});
          return;
        }
        const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded()) {
          send_json(res, 400, {{"error", "body is not valid JSON"}});
          return;
        }
        send_json(res, 200, handler(body));
      };
}

}  // namespace

void register_routes(httplib::Server& server, const Api& api, std::mutex& busy, std::ostream* log) {
  server.set_payload_max_length(std::size_t{64} * 1024);  // requests are a few hundred bytes

  server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, {{"status", "ok"}});
  });
  server.Get("/api/traces", [&api](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, api.traces());
  });
  server.Get("/api/policies", [&api](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, api.policies());
  });
  server.Post("/api/simulate",
              heavy_post(busy, [&api](const json& body) { return api.simulate(body); }));
  server.Post("/api/visualize-window",
              heavy_post(busy, [&api](const json& body) { return api.visualize_window(body); }));

  server.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, const std::exception_ptr& ep) {
        try {
          std::rethrow_exception(ep);
        } catch (const ApiError& e) {
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
  if (log != nullptr) {
    server.set_logger([log](const httplib::Request& req, const httplib::Response& res) {
      *log << req.method << ' ' << req.path << ' ' << res.status << '\n';
    });
  }
}

}  // namespace lcache::api
