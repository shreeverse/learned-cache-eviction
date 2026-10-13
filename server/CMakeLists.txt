# The RESP codec has no Asio dependency (decisions D22) so it can be tested on its own; the
# networking code added later links lc_asio.
add_library(lc_server
  resp.cpp)
target_include_directories(lc_server PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_server PRIVATE lc_warnings)
