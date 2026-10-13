# The RESP codec, the store and the command dispatcher have no Asio dependency (decisions D22),
# so they can be tested on their own; the networking code is a separate library.
add_library(lc_server
  resp.cpp
  store.cpp
  commands.cpp)
target_include_directories(lc_server PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_server PUBLIC lc_policy lc_trace PRIVATE lc_warnings)

add_library(lc_server_net
  connection.cpp
  server.cpp)
target_link_libraries(lc_server_net PUBLIC lc_server lc_asio PRIVATE lc_warnings)

add_executable(lcache-server main.cpp)
target_link_libraries(lcache-server PRIVATE lc_server_net lc_cachesim_lib lc_ml CLI11::CLI11
                      lc_warnings)
