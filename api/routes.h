#pragma once

#include <httplib.h>

#include <mutex>
#include <ostream>

#include "api/api.h"

namespace lcache::api {

// Registers the API's routes on `server`, plus JSON error replies ({"error": "..."}) for
// exceptions, unknown routes and oversized bodies:
//
//   GET  /api/health            {"status": "ok"}
//   GET  /api/traces            Api::traces()
//   GET  /api/policies          Api::policies()
//   POST /api/simulate          Api::simulate()
//   POST /api/visualize-window  Api::visualize_window()
//
// The two POSTs are heavy: `busy` lets one run at a time and a second one gets 429 (decisions
// D26). If `log` is not null, one line per request is written to it from httplib's worker
// threads, so it must tolerate concurrent writes (std::cerr does). `api`, `busy` and `log` must
// outlive the server.
void register_routes(httplib::Server& server, const Api& api, std::mutex& busy, std::ostream* log);

}  // namespace lcache::api
