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
