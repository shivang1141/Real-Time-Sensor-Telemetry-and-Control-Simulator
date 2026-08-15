#include "telemetry/processing/events.hpp"

namespace telemetry {

namespace {
constexpr const char* kComponent = "processing";

LogLevel log_level_for(Severity severity) {
    switch (severity) {
        case Severity::Info:
            return LogLevel::Info;
        case Severity::Warning:
            return LogLevel::Warn;
        case Severity::Critical:
            return LogLevel::Error;
    }
    return LogLevel::Info;
}
}  // namespace

Severity severity_of(Condition condition) {
    switch (condition) {
        case Condition::CritLow:
        case Condition::CritHigh:
        case Condition::SensorFault:
        case Condition::Stale:
            return Severity::Critical;
        case Condition::WarnLow:
        case Condition::WarnHigh:
        case Condition::RateOfChange:
        case Condition::Trend:
        case Condition::SensorDegraded:
            return Severity::Warning;
    }
    return Severity::Warning;
}

std::string_view to_string(Condition condition) {
    switch (condition) {
        case Condition::WarnLow:
            return "warn_low";
        case Condition::WarnHigh:
            return "warn_high";
        case Condition::CritLow:
            return "crit_low";
        case Condition::CritHigh:
            return "crit_high";
        case Condition::RateOfChange:
            return "rate_of_change";
        case Condition::Trend:
            return "trend";
        case Condition::SensorDegraded:
            return "sensor_degraded";
        case Condition::SensorFault:
            return "sensor_fault";
        case Condition::Stale:
            return "stale";
    }
    return "unknown";
}

std::string_view to_string(Severity severity) {
    switch (severity) {
        case Severity::Info:
            return "info";
        case Severity::Warning:
            return "warning";
        case Severity::Critical:
            return "critical";
    }
    return "unknown";
}

std::string_view to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
            return "none";
        case RejectReason::OutOfPhysicalRange:
            return "out_of_physical_range";
        case RejectReason::TimestampInFuture:
            return "timestamp_in_future";
        case RejectReason::TimestampTooOld:
            return "timestamp_too_old";
        case RejectReason::TimestampRegressed:
            return "timestamp_regressed";
        case RejectReason::SequenceRegressed:
            return "sequence_regressed";
        case RejectReason::TypeMismatch:
            return "type_mismatch";
    }
    return "unknown";
}

std::string_view to_string(EventKind kind) {
    switch (kind) {
        case EventKind::ConditionRaised:
            return "condition_raised";
        case EventKind::ConditionCleared:
            return "condition_cleared";
        case EventKind::SequenceGap:
            return "sequence_gap";
        case EventKind::MeasurementRejected:
            return "measurement_rejected";
        case EventKind::SensorRegistered:
            return "sensor_registered";
        case EventKind::SensorRestarted:
            return "sensor_restarted";
    }
    return "unknown";
}

LoggingEventSink::LoggingEventSink(Logger& logger)
    : logger_(logger),
      per_message_limiter_(20, std::chrono::seconds(1)),
      transition_limiter_(200, std::chrono::seconds(1)) {}

void LoggingEventSink::emit(const Event& event) {
    std::uint64_t suppressed = 0;
    switch (event.kind) {
        case EventKind::ConditionRaised: {
            raised_.fetch_add(1, std::memory_order_relaxed);
            const LogLevel level = log_level_for(event.severity);
            if (logger_.enabled(level) && transition_limiter_.allow(suppressed)) {
                logger_.log(level, kComponent, to_string(event.kind),
                            {{"sensor", event.sensor_id},
                             {"condition", to_string(*event.condition)},
                             {"severity", to_string(event.severity)},
                             {"value", event.value},
                             {"suppressed", suppressed}});
            }
            return;
        }
        case EventKind::ConditionCleared:
            cleared_.fetch_add(1, std::memory_order_relaxed);
            if (logger_.enabled(LogLevel::Info) && transition_limiter_.allow(suppressed)) {
                logger_.info(kComponent, to_string(event.kind),
                             {{"sensor", event.sensor_id},
                              {"condition", to_string(*event.condition)},
                              {"value", event.value},
                              {"suppressed", suppressed}});
            }
            return;
        case EventKind::SensorRegistered:
        case EventKind::SensorRestarted:
            logger_.info(kComponent, to_string(event.kind), {{"sensor", event.sensor_id}});
            return;
        case EventKind::SequenceGap:
        case EventKind::MeasurementRejected:
            break;
    }

    if (!logger_.enabled(LogLevel::Warn) || !per_message_limiter_.allow(suppressed)) {
        return;
    }
    if (event.kind == EventKind::SequenceGap) {
        logger_.warn(kComponent, to_string(event.kind),
                     {{"sensor", event.sensor_id}, {"missing", event.count},
                      {"suppressed", suppressed}});
    } else {
        logger_.warn(kComponent, to_string(event.kind),
                     {{"sensor", event.sensor_id}, {"reason", to_string(event.reject_reason)},
                      {"value", event.value}, {"suppressed", suppressed}});
    }
}

}  // namespace telemetry
