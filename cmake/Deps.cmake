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

# Standalone (non-Boost) Asio for the network server (decisions D22). Header-only and the repo
# has no CMakeLists.txt, so MakeAvailable just downloads it and we wrap it in a target.
FetchContent_Declare(asio
  URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
  URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8)
FetchContent_MakeAvailable(asio)
find_package(Threads REQUIRED)
add_library(lc_asio INTERFACE)
# SYSTEM keeps our strict warnings off Asio's headers.
target_include_directories(lc_asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(lc_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(lc_asio INTERFACE Threads::Threads)

# The simulation API (decisions D26): cpp-httplib (single header) and nlohmann/json. Both ship a
# CMakeLists.txt that we skip (SOURCE_SUBDIR points nowhere): httplib's would link OpenSSL, zlib
# or brotli if it happened to find them. We wrap the headers ourselves, as SYSTEM includes.
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  URL_HASH SHA256=a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e
  SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(httplib)
add_library(lc_httplib INTERFACE)
target_include_directories(lc_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
target_link_libraries(lc_httplib INTERFACE Threads::Threads)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(nlohmann_json)
add_library(lc_json INTERFACE)
target_include_directories(lc_json SYSTEM INTERFACE ${nlohmann_json_SOURCE_DIR}/single_include)
