#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "telemetry/common/logger.hpp"

namespace telemetry {

enum class Severity : std::uint8_t { Info, Warning, Critical };

/// Persistent per-sensor conditions. Each is a bit so a sensor's active set fits in one
/// integer. An event is emitted when a condition becomes active and again when it clears
/// (edge-triggered), so a sensor sitting above a threshold does not flood the log.
enum class Condition : std::uint16_t {
    WarnLow = 1u << 0,
    WarnHigh = 1u << 1,
    CritLow = 1u << 2,
    CritHigh = 1u << 3,
    RateOfChange = 1u << 4,
    Trend = 1u << 5,
    SensorDegraded = 1u << 6,
    SensorFault = 1u << 7,
    Stale = 1u << 8,
};

using ConditionSet = std::uint16_t;

inline constexpr std::array<Condition, 9> kAllConditions = {
    Condition::WarnLow,      Condition::WarnHigh, Condition::CritLow,
    Condition::CritHigh,     Condition::RateOfChange, Condition::Trend,
    Condition::SensorDegraded, Condition::SensorFault, Condition::Stale};

constexpr ConditionSet bit(Condition c) { return static_cast<ConditionSet>(c); }
constexpr bool has(ConditionSet set, Condition c) { return (set & bit(c)) != 0; }

Severity severity_of(Condition condition);
std::string_view to_string(Condition condition);
std::string_view to_string(Severity severity);

/// Why a syntactically valid message was rejected by semantic validation.
enum class RejectReason : std::uint8_t {
    None,
    OutOfPhysicalRange,
    TimestampInFuture,
    TimestampTooOld,
    TimestampRegressed,
    SequenceRegressed,
    TypeMismatch,
};
std::string_view to_string(RejectReason reason);

enum class EventKind : std::uint8_t {
    ConditionRaised,
    ConditionCleared,
    SequenceGap,
    MeasurementRejected,
    SensorRegistered,
    SensorRestarted,
};
std::string_view to_string(EventKind kind);

struct Event {
    EventKind kind{EventKind::ConditionRaised};
    Severity severity{Severity::Info};
    std::string sensor_id;
    std::optional<Condition> condition;
    RejectReason reject_reason{RejectReason::None};
    double value{0.0};
    std::uint64_t count{0};  ///< Missing messages for SequenceGap.
    std::int64_t wall_time_us{0};
};

/// Receives events from processing workers. Implementations must be thread-safe.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(const Event& event) = 0;
};

/// Logs events and keeps counters. Every event is counted, but logging is rate-limited:
/// per-message events (rejections, sequence gaps) to 20 lines/s and condition
/// transitions to 200 lines/s. Transitions are edge-triggered, but a sensor oscillating
/// around a limit at a high message rate can still produce thousands per second, and
/// profiling showed such a log flood dominating server CPU. Each line that follows a
/// suppressed burst reports how many lines were suppressed.
class LoggingEventSink final : public EventSink {
public:
    explicit LoggingEventSink(Logger& logger);
    void emit(const Event& event) override;

    std::uint64_t raised() const { return raised_.load(std::memory_order_relaxed); }
    std::uint64_t cleared() const { return cleared_.load(std::memory_order_relaxed); }

private:
    Logger& logger_;
    RateLimiter per_message_limiter_;
    RateLimiter transition_limiter_;
    std::atomic<std::uint64_t> raised_{0};
    std::atomic<std::uint64_t> cleared_{0};
};

}  // namespace telemetry
