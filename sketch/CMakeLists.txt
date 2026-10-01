add_library(lc_sketch
  count_min_sketch.cpp)
target_include_directories(lc_sketch PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_sketch PUBLIC lc_trace PRIVATE lc_warnings)
