add_library(telemetry_core
    common/logger.cpp
    common/telemetry_message.cpp
    protocol/protocol.cpp
)

target_include_directories(telemetry_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(telemetry_core
    PUBLIC Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json telemetry_warnings)
