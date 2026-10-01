include(FetchContent)

# Pinned by tarball hash for reproducible builds.
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
# Discover tests when ctest runs, not at build time: the TSan binaries must be started under
# `setarch -R` on this kernel (see docs/decisions.md, D5), which a build step can't do.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

FetchContent_Declare(cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
  URL_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a)
FetchContent_MakeAvailable(cli11)
