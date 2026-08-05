include(GoogleTest)

add_library(telemetry_test_support INTERFACE)
target_include_directories(telemetry_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(unit_tests
    unit/test_protocol.cpp
    unit/test_line_framer.cpp
)

foreach(target unit_tests)
    target_link_libraries(${target} PRIVATE
        telemetry_core telemetry_test_support GTest::gtest_main nlohmann_json::nlohmann_json)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_compile_definitions(${target} PRIVATE TELEMETRY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endforeach()

gtest_discover_tests(unit_tests PROPERTIES LABELS unit DISCOVERY_TIMEOUT 60)
