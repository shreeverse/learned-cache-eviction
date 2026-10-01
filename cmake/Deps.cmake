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

if(LC_WITH_LIGHTGBM)
  # LightGBM 4.5.0, the newest release that builds with CMake 3.22 (decisions D16). The Python
  # sdist contains the full C++ sources and vendored Eigen/fmt/fast_double_parser.
  FetchContent_Declare(lightgbm
    URL https://github.com/lightgbm-org/LightGBM/releases/download/v4.5.0/lightgbm-4.5.0.tar.gz
    URL_HASH SHA256=e1cd7baf0318d4e308a26575a63a4635f08df866ad3622a9d8e3d71d9637a1ba)
  # No OpenMP: one small batch per eviction doesn't need threads, and LightGBM's OpenMP pool
  # gives TSan false positives (D14). Static, library only.
  set(USE_OPENMP OFF CACHE BOOL "" FORCE)
  set(BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
  set(INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lightgbm)
  # LightGBM doesn't export its include directory; SYSTEM keeps our strict warnings off its headers.
  target_include_directories(_lightgbm SYSTEM INTERFACE ${lightgbm_SOURCE_DIR}/include)
endif()

if(LC_BUILD_BENCHMARKS)
  FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)
endif()
