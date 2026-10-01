add_library(lc_policy
  lru.cpp
  lfu.cpp
  arc.cpp
  wtinylfu.cpp
  belady.cpp
  learned.cpp
  fallback.cpp
  policy_factory.cpp)
target_include_directories(lc_policy PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_policy PUBLIC lc_trace lc_sketch lc_ml PRIVATE lc_warnings)
