# Parsing helpers live in a library so they can be unit-tested.
add_library(lc_cachesim_lib size_parse.cpp)
target_include_directories(lc_cachesim_lib PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_cachesim_lib PRIVATE lc_warnings)

add_executable(cachesim main.cpp)
target_link_libraries(cachesim PRIVATE lc_cachesim_lib lc_sim lc_policy lc_trace CLI11::CLI11
                      lc_warnings)
