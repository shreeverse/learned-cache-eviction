add_library(lc_sim
  cache.cpp
  simulator.cpp)
target_include_directories(lc_sim PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_sim PUBLIC lc_policy lc_trace PRIVATE lc_warnings)
