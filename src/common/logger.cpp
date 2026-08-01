#include "telemetry/common/logger.hpp"

#include <ctime>
#include <stdexcept>

namespace telemetry {

namespace {

bool needs_quoting(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    for (char c : value) {
        if (c == ' ' || c == '"' || c == '=' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            return true;
        }
    }
    return false;
}

void append_value(std::string& out, std::string_view value) {
    if (!needs_quoting(value)) {
        out.append(value);
        return;
    }
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out.push_back('?');
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void append_timestamp(std::string& out, std::int64_t wall_us) {
    const std::time_t seconds = static_cast<std::time_t>(wall_us / 1'000'000);
    const auto micros = static_cast<long>(wall_us % 1'000'000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, micros);
    out.append(buffer);
}

}  // namespace

std::string_view to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "unknown";
}

std::optional<LogLevel> parse_log_level(std::string_view text) {
    for (LogLevel level :
         {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Off}) {
        if (to_string(level) == text) {
            return level;
        }
    }
    return std::nullopt;
}

Logger::Logger(LogLevel min_level, std::ostream* sink) : level_(min_level), sink_(sink) {}

void Logger::open_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(path, std::ios::out | std::ios::app);
    if (!file_) {
        throw std::runtime_error("cannot open log file: " + path);
    }
    sink_ = &file_;
}

std::string Logger::format_line(std::int64_t wall_us, LogLevel level, std::string_view component,
                                std::string_view event, std::initializer_list<LogField> fields) {
    std::string line;
    line.reserve(128);
    append_timestamp(line, wall_us);
    line.append(" level=");
    line.append(to_string(level));
    line.append(" component=");
    append_value(line, component);
    line.append(" event=");
    append_value(line, event);
    for (const LogField& field : fields) {
        line.push_back(' ');
        line.append(field.key());
        line.push_back('=');
        append_value(line, field.value());
    }
    return line;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view event,
                 std::initializer_list<LogField> fields) {
    if (!enabled(level)) {
        return;
    }
    // Format outside the lock; only the write is serialized.
    std::string line =
        format_line(SystemClock{}.wall_now_us(), level, component, event, fields);
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    if (sink_ == nullptr) {
        return;
    }
    sink_->write(line.data(), static_cast<std::streamsize>(line.size()));
    if (level >= LogLevel::Warn) {
        sink_->flush();
    }
}

bool RateLimiter::allow(std::uint64_t& suppressed, SteadyTime now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (now - window_start_ >= window_) {
        window_start_ = now;
        in_window_ = 0;
    }
    if (in_window_ < max_per_window_) {
        ++in_window_;
        suppressed = suppressed_;
        suppressed_ = 0;
        return true;
    }
    ++suppressed_;
    return false;
}

}  // namespace telemetry
