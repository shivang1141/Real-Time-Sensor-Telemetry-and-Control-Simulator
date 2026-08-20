#pragma once

#include <stdexcept>
#include <string>

namespace telemetry {

/// Thrown for unreadable, malformed or semantically invalid configuration.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace telemetry
