# Smoke test that the toolchain, GoogleTest and sanitizer wiring work. Module tests are added
# alongside each module.
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE GTest::gtest_main lc_warnings)
gtest_discover_tests(smoke_test)
