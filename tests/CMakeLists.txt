# Smoke test that the toolchain, GoogleTest and sanitizer wiring work.
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE GTest::gtest_main lc_warnings)
gtest_discover_tests(smoke_test)

add_executable(trace_test
  trace/oracle_general_reader_test.cpp
  trace/csv_reader_test.cpp)
target_include_directories(trace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(trace_test PRIVATE lc_trace GTest::gtest_main lc_warnings)
gtest_discover_tests(trace_test)

add_executable(policy_test
  policy/lru_test.cpp)
target_include_directories(policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(policy_test PRIVATE lc_policy GTest::gtest_main lc_warnings)
gtest_discover_tests(policy_test)

add_executable(sim_test
  sim/cache_test.cpp
  sim/simulator_test.cpp)
target_include_directories(sim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sim_test PRIVATE lc_sim GTest::gtest_main lc_warnings)
gtest_discover_tests(sim_test)
