cmake_minimum_required(VERSION 3.22)

project(sensor_telemetry_simulator
    VERSION 1.0.0
    DESCRIPTION "Real-time sensor telemetry processing simulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(TELEMETRY_BUILD_TESTS "Build unit and integration tests" ON)
option(TELEMETRY_BUILD_BENCHMARKS "Build benchmark tools" ON)
option(TELEMETRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(TELEMETRY_SANITIZER "" CACHE STRING
    "Sanitizer build: empty, 'address' (ASan + UBSan) or 'thread' (TSan)")

find_package(Threads REQUIRED)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(src)

if(TELEMETRY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
