#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "telemetry/common/telemetry_message.hpp"

/// Wire protocol: newline-delimited JSON (one object per line, UTF-8, '\n' terminated).
///
///   {"id":"temp-001","seq":42,"status":"ok","ts":1729500000123456,"type":"temperature","value":21.5}
///
///   id      string   required  1..64 chars of [A-Za-z0-9._-]
///   type    string   required  temperature | pressure | motor_speed | signal_strength
///   value   number   required  finite
///   ts      integer  required  sender wall clock, microseconds since Unix epoch, > 0
///   seq     integer  required  unsigned per-sensor sequence number
///   status  string   optional  ok (default) | degraded | fault
///
/// Unknown fields are ignored for forward compatibility. Frames longer than the server's
/// max_frame_bytes are discarded by the framer before they reach the parser.
namespace telemetry::protocol {

inline constexpr std::size_t kDefaultMaxFrameBytes = 1024;
inline constexpr std::size_t kMaxSensorIdLength = 64;

enum class ParseError {
    None,
    Empty,
    MalformedJson,
    NotAnObject,
    MissingField,
    WrongFieldType,
    InvalidSensorId,
    UnknownSensorType,
    UnknownStatus,
    NonFiniteValue,
    InvalidTimestamp,
};

std::string_view to_string(ParseError error);

struct DecodeResult {
    std::optional<TelemetryMessage> message;
    ParseError error{ParseError::None};
    std::string detail;  ///< Human-readable context, e.g. the offending field name.

    bool ok() const { return message.has_value(); }
};

bool is_valid_sensor_id(std::string_view id);

/// Serializes a message as a single JSON line without the trailing newline.
std::string encode(const TelemetryMessage& message);

/// Parses and validates one frame (without its newline). Never throws.
DecodeResult decode(std::string_view frame);

}  // namespace telemetry::protocol
