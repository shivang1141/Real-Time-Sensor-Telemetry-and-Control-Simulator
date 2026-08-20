#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "telemetry/app/config_error.hpp"
#include "telemetry/common/logger.hpp"
#include "telemetry/net/tcp_server.hpp"
#include "telemetry/processing/anomaly_detector.hpp"
#include "telemetry/processing/processing_engine.hpp"

namespace telemetry {

struct LoggingConfig {
    LogLevel level{LogLevel::Info};
    std::string file;  ///< Empty: log to stderr.
};

struct StatusConfig {
    std::chrono::milliseconds interval{1000};  ///< 0 disables periodic status output.
    std::size_t max_listed_sensors{10};
};

struct ServerConfig {
    net::TcpServerConfig network;
    ProcessingConfig processing;
    DetectorConfig detection{DetectorConfig::defaults()};
    LoggingConfig logging;
    StatusConfig status;

    /// Throws ConfigError describing the first inconsistency found.
    void validate() const;
};

/// Loads a server configuration. Every field is optional and defaults to the values in
/// ServerConfig; unknown fields are rejected. Throws ConfigError.
ServerConfig load_server_config(const std::string& path);
ServerConfig parse_server_config(const std::string& json_text);

}  // namespace telemetry
