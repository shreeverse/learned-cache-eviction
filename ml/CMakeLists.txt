add_library(lc_ml
  feature_tracker.cpp)
target_include_directories(lc_ml PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lc_ml PUBLIC lc_trace PRIVATE lc_warnings)
