include(GoogleTest)

add_library(telemetry_test_support INTERFACE)
target_include_directories(telemetry_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(unit_tests
    unit/test_protocol.cpp
    unit/test_line_framer.cpp
    unit/test_bounded_queue.cpp
    unit/test_anomaly_detector.cpp
    unit/test_sensor_registry.cpp
    unit/test_latency_histogram.cpp
)

add_executable(integration_tests
    integration/test_tcp_server.cpp
)

foreach(target unit_tests integration_tests)
    target_link_libraries(${target} PRIVATE
        telemetry_core telemetry_test_support GTest::gtest_main nlohmann_json::nlohmann_json)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_compile_definitions(${target} PRIVATE TELEMETRY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endforeach()

gtest_discover_tests(unit_tests PROPERTIES LABELS unit DISCOVERY_TIMEOUT 60)
gtest_discover_tests(integration_tests PROPERTIES LABELS integration TIMEOUT 60 DISCOVERY_TIMEOUT 60)
