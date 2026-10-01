add_library(lc_policy
  lru.cpp)
target_include_directories(lc_policy PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_policy PUBLIC lc_trace PRIVATE lc_warnings)
