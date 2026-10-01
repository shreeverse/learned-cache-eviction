cmake_minimum_required(VERSION 3.22)
project(learned_cache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LC_BUILD_TESTS "Build unit tests" ON)
option(LC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (on in CI)" OFF)
set(LC_SANITIZER "" CACHE STRING "Sanitizers to enable: '', 'address,undefined', or 'thread'")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Sanitizers)  # must come before any targets (incl. GoogleTest) so everything is instrumented
include(Warnings)

add_subdirectory(trace)
add_subdirectory(policy)

if(LC_BUILD_TESTS)
  enable_testing()
  include(Deps)
  add_subdirectory(tests)
endif()
