# The RESP codec and the store have no Asio dependency (decisions D22), so they can be tested on
# their own; the networking code links lc_asio.
add_library(lc_server
  resp.cpp
  store.cpp)
target_include_directories(lc_server PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_server PUBLIC lc_policy lc_trace PRIVATE lc_warnings)
