add_library(telemetry_core
    common/logger.cpp
    common/json_reader.cpp
    common/telemetry_message.cpp
    protocol/protocol.cpp
    net/socket.cpp
    net/tcp_client.cpp
    net/tcp_server.cpp
    metrics/latency_histogram.cpp
    metrics/metrics.cpp
    processing/events.cpp
    processing/anomaly_detector.cpp
    processing/sensor_registry.cpp
    processing/processing_engine.cpp
    app/server_config.cpp
    app/telemetry_service.cpp
    app/status_reporter.cpp
    sim/sensor_model.cpp
    sim/simulator.cpp
    sim/simulator_config.cpp
)

target_include_directories(telemetry_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(telemetry_core
    PUBLIC Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json telemetry_warnings)
