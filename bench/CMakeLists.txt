# Benchmarks need LightGBM (they time the learned policy) and the Twitter trace at run time.
if(LC_WITH_LIGHTGBM)
  add_executable(eviction_bench eviction_bench.cpp)
  target_compile_definitions(eviction_bench PRIVATE LC_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_link_libraries(eviction_bench PRIVATE lc_sim lc_policy lc_ml lc_trace
                        benchmark::benchmark lc_warnings)
endif()
