#include "telemetry/processing/anomaly_detector.hpp"

#include <cmath>
#include <iterator>
#include <sstream>

namespace telemetry {

std::string Thresholds::validate() const {
    std::ostringstream problem;
    if (!(valid_min <= crit_low && crit_low <= warn_low && warn_low < warn_high &&
          warn_high <= crit_high && crit_high <= valid_max)) {
        problem << "thresholds must satisfy valid_min <= crit_low <= warn_low < warn_high"
                   " <= crit_high <= valid_max";
    } else if (hysteresis < 0 || max_rate_per_s < 0 || max_trend_per_s < 0) {
        problem << "hysteresis, max_rate_per_s and max_trend_per_s must be >= 0";
    }
    return problem.str();
}

DetectorConfig DetectorConfig::defaults() {
    DetectorConfig config;
    // Synthetic, plausible ranges chosen for demonstration only.
    config.for_type(SensorType::Temperature) = {-60, 200, -20, 0, 85, 105, 1.0, 20, 2.0};
    config.for_type(SensorType::Pressure) = {0, 500, 80, 90, 115, 130, 1.0, 30, 5.0};
    config.for_type(SensorType::MotorSpeed) = {0, 20000, 200, 1000, 6000, 7500, 50, 2000, 300};
    config.for_type(SensorType::SignalStrength) = {-130, 0, -100, -85, -20, -10, 1.0, 30, 5.0};
    return config;
}

RejectReason AnomalyDetector::validate(const TelemetryMessage& message,
                                       const TelemetryMessage* previous,
                                       std::int64_t receiver_wall_us) const {
    if (previous != nullptr && previous->type != message.type) {
        return RejectReason::TypeMismatch;
    }
    const Thresholds& limits = config_.for_type(message.type);
    if (message.value < limits.valid_min || message.value > limits.valid_max) {
        return RejectReason::OutOfPhysicalRange;
    }
    const auto skew_us =
        std::chrono::duration_cast<std::chrono::microseconds>(config_.max_future_skew).count();
    const auto age_us =
        std::chrono::duration_cast<std::chrono::microseconds>(config_.max_message_age).count();
    if (message.timestamp_us > receiver_wall_us + skew_us) {
        return RejectReason::TimestampInFuture;
    }
    if (message.timestamp_us < receiver_wall_us - age_us) {
        return RejectReason::TimestampTooOld;
    }
    if (previous != nullptr) {
        if (message.sequence <= previous->sequence) {
            return RejectReason::SequenceRegressed;
        }
        if (message.timestamp_us < previous->timestamp_us) {
            return RejectReason::TimestampRegressed;
        }
    }
    return RejectReason::None;
}

ConditionSet AnomalyDetector::evaluate(const TelemetryMessage& message,
                                       const std::deque<Sample>& history,
                                       ConditionSet active) const {
    const Thresholds& limits = config_.for_type(message.type);
    const double v = message.value;
    const double h = limits.hysteresis;
    ConditionSet result = 0;

    // Limit checks with hysteresis: once raised, a condition holds until the value is
    // back inside the limit by `hysteresis`, which prevents flapping around a threshold.
    auto check = [&](Condition condition, bool beyond_limit, bool beyond_clear_point) {
        const bool raised = has(active, condition) ? beyond_clear_point : beyond_limit;
        if (raised) {
            result |= bit(condition);
        }
    };
    check(Condition::WarnHigh, v > limits.warn_high, v > limits.warn_high - h);
    check(Condition::CritHigh, v > limits.crit_high, v > limits.crit_high - h);
    check(Condition::WarnLow, v < limits.warn_low, v < limits.warn_low + h);
    check(Condition::CritLow, v < limits.crit_low, v < limits.crit_low + h);

    if (limits.max_rate_per_s > 0 && history.size() >= 2) {
        const Sample& cur = history.back();
        const double target_t = cur.t_s - std::chrono::duration<double>(config_.rate_interval).count();
        const Sample* reference = &history.front();
        for (auto it = std::next(history.rbegin()); it != history.rend(); ++it) {
            if (it->t_s <= target_t) {
                reference = &*it;
                break;
            }
        }
        const double dt = cur.t_s - reference->t_s;
        if (dt > 0 && std::fabs(cur.value - reference->value) / dt > limits.max_rate_per_s) {
            result |= bit(Condition::RateOfChange);
        }
    }

    if (limits.max_trend_per_s > 0 && history.size() >= config_.min_trend_samples) {
        const auto slope = trend_slope(history);
        if (slope && std::fabs(*slope) > limits.max_trend_per_s) {
            result |= bit(Condition::Trend);
        }
    }

    if (message.status == SensorStatus::Degraded) {
        result |= bit(Condition::SensorDegraded);
    } else if (message.status == SensorStatus::Fault) {
        result |= bit(Condition::SensorFault);
    }
    return result;
}

std::optional<double> AnomalyDetector::trend_slope(const std::deque<Sample>& samples) {
    const std::size_t n = samples.size();
    if (n < 2) {
        return std::nullopt;
    }
    // Center on the means before accumulating: timestamps are ~1.7e9 s, and squaring
    // them directly would destroy precision.
    double mean_t = 0;
    double mean_v = 0;
    for (const Sample& s : samples) {
        mean_t += s.t_s;
        mean_v += s.value;
    }
    mean_t /= static_cast<double>(n);
    mean_v /= static_cast<double>(n);

    double sxx = 0;
    double sxy = 0;
    for (const Sample& s : samples) {
        const double dt = s.t_s - mean_t;
        sxx += dt * dt;
        sxy += dt * (s.value - mean_v);
    }
    if (sxx <= 0) {
        return std::nullopt;
    }
    return sxy / sxx;
}

}  // namespace telemetry
