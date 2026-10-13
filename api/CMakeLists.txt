# The API's logic has no HTTP dependency (decisions D26), so it is tested without sockets.
add_library(lc_api api.cpp)
target_include_directories(lc_api PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_api PUBLIC lc_sim lc_policy lc_trace lc_json PRIVATE lc_ml lc_warnings)

add_executable(lcache-api main.cpp)
target_link_libraries(lcache-api PRIVATE lc_api lc_httplib CLI11::CLI11 lc_warnings)
