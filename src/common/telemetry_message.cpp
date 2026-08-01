#include "telemetry/common/telemetry_message.hpp"

namespace telemetry {

std::string_view to_string(SensorType type) {
    switch (type) {
        case SensorType::Temperature:
            return "temperature";
        case SensorType::Pressure:
            return "pressure";
        case SensorType::MotorSpeed:
            return "motor_speed";
        case SensorType::SignalStrength:
            return "signal_strength";
    }
    return "unknown";
}

std::string_view to_string(SensorStatus status) {
    switch (status) {
        case SensorStatus::Ok:
            return "ok";
        case SensorStatus::Degraded:
            return "degraded";
        case SensorStatus::Fault:
            return "fault";
    }
    return "unknown";
}

std::string_view units(SensorType type) {
    switch (type) {
        case SensorType::Temperature:
            return "degC";
        case SensorType::Pressure:
            return "kPa";
        case SensorType::MotorSpeed:
            return "rpm";
        case SensorType::SignalStrength:
            return "dBm";
    }
    return "";
}

std::optional<SensorType> parse_sensor_type(std::string_view text) {
    for (SensorType type : kAllSensorTypes) {
        if (to_string(type) == text) {
            return type;
        }
    }
    return std::nullopt;
}

std::optional<SensorStatus> parse_sensor_status(std::string_view text) {
    for (SensorStatus status : {SensorStatus::Ok, SensorStatus::Degraded, SensorStatus::Fault}) {
        if (to_string(status) == text) {
            return status;
        }
    }
    return std::nullopt;
}

}  // namespace telemetry
