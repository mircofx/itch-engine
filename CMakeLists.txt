cmake_minimum_required(VERSION 3.25)
project(itch_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ITCH_NATIVE      "Build with -march=native (non-portable)" ON)
option(ITCH_BUILD_TESTS "Build Catch2 tests"                      ON)
option(ITCH_BUILD_BENCH "Build Google Benchmark suite"           ON)

# Warnings: safe for every target we own, incl. tests/bench.
add_library(itch_warnings INTERFACE)
target_compile_options(itch_warnings INTERFACE -Wall -Wextra -Wpedantic)

# Hot-path flags: engine/library ONLY. -fno-exceptions must NOT be global or
# it breaks Catch2/benchmark, so it lives here and never reaches the deps.
add_library(itch_flags INTERFACE)
target_link_libraries(itch_flags INTERFACE itch_warnings)
target_compile_options(itch_flags INTERFACE
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-g>)
if(ITCH_NATIVE)
  target_compile_options(itch_flags INTERFACE -march=native)
endif()

add_executable(itch_engine src/replay/main.cpp)
target_include_directories(itch_engine PRIVATE src)
target_link_libraries(itch_engine PRIVATE itch_flags)

include(FetchContent)
if(ITCH_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        URL ${CMAKE_SOURCE_DIR}/third_party/Catch2-devel.zip
    )

    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

    enable_testing()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
        add_subdirectory(tests)
    endif()
endif()

if(ITCH_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        benchmark
        URL ${CMAKE_SOURCE_DIR}/third_party/benchmark-main.zip
    )

    FetchContent_MakeAvailable(benchmark)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
        add_subdirectory(bench)
    endif()
endif()