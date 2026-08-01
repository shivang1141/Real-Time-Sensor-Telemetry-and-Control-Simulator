#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "telemetry/common/clock.hpp"

namespace telemetry {

enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error, Off };

std::string_view to_string(LogLevel level);
std::optional<LogLevel> parse_log_level(std::string_view text);

/// One key=value pair of a structured log line.
class LogField {
public:
    LogField(std::string_view key, std::string_view value) : key_(key), value_(value) {}
    LogField(std::string_view key, const char* value) : key_(key), value_(value) {}
    LogField(std::string_view key, const std::string& value) : key_(key), value_(value) {}

    template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
    LogField(std::string_view key, T value) : key_(key), value_(format_number(value)) {}

    std::string_view key() const { return key_; }
    const std::string& value() const { return value_; }

private:
    template <typename T>
    static std::string format_number(T value) {
        if constexpr (std::is_same_v<T, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_integral_v<T>) {
            return std::to_string(value);
        } else {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.6g", static_cast<double>(value));
            return buffer;
        }
    }

    std::string_view key_;
    std::string value_;
};

/// Thread-safe structured logger writing logfmt lines:
///   2026-08-01T12:00:00.123456Z level=info component=net event=client_connected conn=3
///
/// A Logger is an ordinary object passed by reference to the components that need it;
/// there is no global logger. Use LogLevel::Off to silence it (tests, benchmarks).
class Logger {
public:
    explicit Logger(LogLevel min_level = LogLevel::Info, std::ostream* sink = &std::cerr);

    /// Redirects output to an append-mode file. Throws std::runtime_error on failure.
    void open_file(const std::string& path);

    void set_level(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }
    bool enabled(LogLevel level) const {
        return level != LogLevel::Off && level >= level_.load(std::memory_order_relaxed);
    }

    void log(LogLevel level, std::string_view component, std::string_view event,
             std::initializer_list<LogField> fields = {});

    void debug(std::string_view component, std::string_view event,
               std::initializer_list<LogField> fields = {}) {
        log(LogLevel::Debug, component, event, fields);
    }
    void info(std::string_view component, std::string_view event,
              std::initializer_list<LogField> fields = {}) {
        log(LogLevel::Info, component, event, fields);
    }
    void warn(std::string_view component, std::string_view event,
              std::initializer_list<LogField> fields = {}) {
        log(LogLevel::Warn, component, event, fields);
    }
    void error(std::string_view component, std::string_view event,
               std::initializer_list<LogField> fields = {}) {
        log(LogLevel::Error, component, event, fields);
    }

    /// Formats one line without the trailing newline. Exposed for tests.
    static std::string format_line(std::int64_t wall_us, LogLevel level,
                                   std::string_view component, std::string_view event,
                                   std::initializer_list<LogField> fields);

private:
    std::atomic<LogLevel> level_;
    std::mutex mutex_;
    std::ostream* sink_;
    std::ofstream file_;
};

/// Limits how often a high-volume condition is logged (e.g. one line per invalid
/// message under a flood). Counts suppressed occurrences so they can be reported.
/// Thread-safe.
class RateLimiter {
public:
    RateLimiter(std::uint32_t max_per_window, std::chrono::milliseconds window)
        : max_per_window_(max_per_window), window_(window) {}

    /// Returns true if this occurrence may be logged. On true, `suppressed` is set to
    /// the number of occurrences dropped since the previous allowed one.
    bool allow(std::uint64_t& suppressed, SteadyTime now = SteadyClock::now());

private:
    std::mutex mutex_;
    std::uint32_t max_per_window_;
    std::chrono::milliseconds window_;
    SteadyTime window_start_{};
    std::uint32_t in_window_{0};
    std::uint64_t suppressed_{0};
};

}  // namespace telemetry
