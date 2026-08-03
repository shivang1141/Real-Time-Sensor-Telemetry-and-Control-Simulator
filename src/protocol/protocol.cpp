#include "telemetry/protocol/protocol.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace telemetry::protocol {

namespace {

using nlohmann::json;

DecodeResult failure(ParseError error, std::string detail) {
    return DecodeResult{std::nullopt, error, std::move(detail)};
}

}  // namespace

std::string_view to_string(ParseError error) {
    switch (error) {
        case ParseError::None:
            return "none";
        case ParseError::Empty:
            return "empty";
        case ParseError::MalformedJson:
            return "malformed_json";
        case ParseError::NotAnObject:
            return "not_an_object";
        case ParseError::MissingField:
            return "missing_field";
        case ParseError::WrongFieldType:
            return "wrong_field_type";
        case ParseError::InvalidSensorId:
            return "invalid_sensor_id";
        case ParseError::UnknownSensorType:
            return "unknown_sensor_type";
        case ParseError::UnknownStatus:
            return "unknown_status";
        case ParseError::NonFiniteValue:
            return "non_finite_value";
        case ParseError::InvalidTimestamp:
            return "invalid_timestamp";
    }
    return "unknown";
}

bool is_valid_sensor_id(std::string_view id) {
    if (id.empty() || id.size() > kMaxSensorIdLength) {
        return false;
    }
    for (char c : id) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string encode(const TelemetryMessage& message) {
    const json object = {
        {"id", message.sensor_id},
        {"type", std::string(to_string(message.type))},
        {"value", message.value},
        {"ts", message.timestamp_us},
        {"seq", message.sequence},
        {"status", std::string(to_string(message.status))},
    };
    return object.dump();
}

DecodeResult decode(std::string_view frame) {
    if (frame.empty()) {
        return failure(ParseError::Empty, "empty frame");
    }

    const json object = json::parse(frame.begin(), frame.end(), nullptr,
                                    /*allow_exceptions=*/false);
    if (object.is_discarded()) {
        return failure(ParseError::MalformedJson, "invalid JSON");
    }
    if (!object.is_object()) {
        return failure(ParseError::NotAnObject, "top-level value is not an object");
    }

    auto field = [&object](const char* name) -> const json* {
        const auto it = object.find(name);
        return it == object.end() ? nullptr : &*it;
    };

    TelemetryMessage message;

    const json* id = field("id");
    if (id == nullptr) {
        return failure(ParseError::MissingField, "id");
    }
    if (!id->is_string()) {
        return failure(ParseError::WrongFieldType, "id");
    }
    message.sensor_id = id->get<std::string>();
    if (!is_valid_sensor_id(message.sensor_id)) {
        return failure(ParseError::InvalidSensorId, "id");
    }

    const json* type = field("type");
    if (type == nullptr) {
        return failure(ParseError::MissingField, "type");
    }
    if (!type->is_string()) {
        return failure(ParseError::WrongFieldType, "type");
    }
    const auto parsed_type = parse_sensor_type(type->get_ref<const std::string&>());
    if (!parsed_type) {
        return failure(ParseError::UnknownSensorType, type->get<std::string>());
    }
    message.type = *parsed_type;

    const json* value = field("value");
    if (value == nullptr) {
        return failure(ParseError::MissingField, "value");
    }
    if (!value->is_number()) {
        return failure(ParseError::WrongFieldType, "value");
    }
    message.value = value->get<double>();
    if (!std::isfinite(message.value)) {
        return failure(ParseError::NonFiniteValue, "value");
    }

    // Non-negative JSON integers are stored as unsigned by nlohmann; negative values and
    // floats are rejected here.
    const json* ts = field("ts");
    if (ts == nullptr) {
        return failure(ParseError::MissingField, "ts");
    }
    if (!ts->is_number_unsigned()) {
        return failure(ParseError::InvalidTimestamp, "ts must be a positive integer");
    }
    const auto ts_value = ts->get<std::uint64_t>();
    if (ts_value == 0 ||
        ts_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(ParseError::InvalidTimestamp, "ts out of range");
    }
    message.timestamp_us = static_cast<std::int64_t>(ts_value);

    const json* seq = field("seq");
    if (seq == nullptr) {
        return failure(ParseError::MissingField, "seq");
    }
    if (!seq->is_number_unsigned()) {
        return failure(ParseError::WrongFieldType, "seq");
    }
    message.sequence = seq->get<std::uint64_t>();

    if (const json* status = field("status"); status != nullptr) {
        if (!status->is_string()) {
            return failure(ParseError::WrongFieldType, "status");
        }
        const auto parsed_status = parse_sensor_status(status->get_ref<const std::string&>());
        if (!parsed_status) {
            return failure(ParseError::UnknownStatus, status->get<std::string>());
        }
        message.status = *parsed_status;
    }

    return DecodeResult{std::move(message), ParseError::None, {}};
}

}  // namespace telemetry::protocol
