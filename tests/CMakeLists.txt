# Smoke test that the toolchain, GoogleTest and sanitizer wiring work.
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE GTest::gtest_main lc_warnings)
gtest_discover_tests(smoke_test)

add_executable(trace_test
  trace/oracle_general_reader_test.cpp
  trace/csv_reader_test.cpp
  trace/next_access_test.cpp)
target_include_directories(trace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(trace_test PRIVATE lc_trace GTest::gtest_main lc_warnings)
gtest_discover_tests(trace_test)

add_executable(policy_test
  policy/lru_test.cpp
  policy/lfu_test.cpp
  policy/arc_test.cpp
  policy/wtinylfu_test.cpp
  policy/belady_test.cpp
  policy/learned_test.cpp
  policy/policy_factory_test.cpp
  policy/collector_test.cpp)
target_include_directories(policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# lc_sim: differential tests drive policies through the real Cache.
target_link_libraries(policy_test PRIVATE lc_policy lc_sim GTest::gtest_main lc_warnings)
gtest_discover_tests(policy_test)

add_executable(sim_test
  sim/cache_test.cpp
  sim/simulator_test.cpp
  sim/libcachesim_golden_test.cpp)
target_include_directories(sim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sim_test PRIVATE LC_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
target_link_libraries(sim_test PRIVATE lc_sim GTest::gtest_main lc_warnings)
gtest_discover_tests(sim_test)

add_executable(apps_test
  apps/size_parse_test.cpp)
target_link_libraries(apps_test PRIVATE lc_cachesim_lib GTest::gtest_main lc_warnings)
gtest_discover_tests(apps_test)

add_executable(sketch_test
  sketch/count_min_sketch_test.cpp)
target_include_directories(sketch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sketch_test PRIVATE lc_sketch GTest::gtest_main lc_warnings)
gtest_discover_tests(sketch_test)

add_executable(ml_test
  ml/feature_tracker_test.cpp
  ml/predictor_test.cpp
  ml/tree_ensemble_test.cpp)
target_include_directories(ml_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ml_test PRIVATE LC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(ml_test PRIVATE lc_ml GTest::gtest_main lc_warnings)
gtest_discover_tests(ml_test)

if(LC_WITH_LIGHTGBM)
  add_executable(lightgbm_test
    ml/lightgbm_predictor_test.cpp)
  target_include_directories(lightgbm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(lightgbm_test PRIVATE LC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                               LC_MODELS_DIR="${PROJECT_SOURCE_DIR}/models")
  target_link_libraries(lightgbm_test PRIVATE lc_ml lc_policy lc_sim GTest::gtest_main lc_warnings)
  gtest_discover_tests(lightgbm_test)
endif()
