#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace telemetry {

/// Kinds of synthetic sensors supported by the simulator and the processing engine.
enum class SensorType : std::uint8_t { Temperature, Pressure, MotorSpeed, SignalStrength };

inline constexpr std::size_t kSensorTypeCount = 4;
inline constexpr std::array<SensorType, kSensorTypeCount> kAllSensorTypes = {
    SensorType::Temperature, SensorType::Pressure, SensorType::MotorSpeed,
    SensorType::SignalStrength};

/// Status self-reported by the sensor in each message.
enum class SensorStatus : std::uint8_t { Ok, Degraded, Fault };

std::string_view to_string(SensorType type);
std::string_view to_string(SensorStatus status);
std::string_view units(SensorType type);
std::optional<SensorType> parse_sensor_type(std::string_view text);
std::optional<SensorStatus> parse_sensor_status(std::string_view text);

constexpr std::size_t index_of(SensorType type) { return static_cast<std::size_t>(type); }

/// One measurement as produced by a sensor and carried on the wire.
struct TelemetryMessage {
    std::string sensor_id;
    SensorType type{SensorType::Temperature};
    double value{0.0};
    std::int64_t timestamp_us{0};  ///< Sender wall clock, microseconds since the Unix epoch.
    std::uint64_t sequence{0};     ///< Per-sensor counter: starts at 0, +1 per generated sample.
    SensorStatus status{SensorStatus::Ok};
};

}  // namespace telemetry
