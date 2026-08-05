#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "telemetry/common/clock.hpp"
#include "telemetry/common/telemetry_message.hpp"

namespace telemetry::test {

/// Polls `predicate` until it is true or `timeout` elapses. Returns the final result.
/// Used instead of fixed sleeps so tests are fast when things work and robust on slow
/// (e.g. sanitizer) builds.
template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

inline TelemetryMessage make_message(std::string id, std::uint64_t seq, double value,
                                     std::int64_t ts_us,
                                     SensorType type = SensorType::Temperature,
                                     SensorStatus status = SensorStatus::Ok) {
    TelemetryMessage m;
    m.sensor_id = std::move(id);
    m.type = type;
    m.value = value;
    m.timestamp_us = ts_us;
    m.sequence = seq;
    m.status = status;
    return m;
}

inline std::int64_t now_us() { return SystemClock{}.wall_now_us(); }

}  // namespace telemetry::test
