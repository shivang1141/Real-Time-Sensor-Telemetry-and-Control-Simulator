#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "telemetry/common/telemetry_message.hpp"
#include "telemetry/processing/events.hpp"

namespace telemetry {

/// Limits for one sensor type. Required ordering (checked by validate()):
///   valid_min <= crit_low <= warn_low < warn_high <= crit_high <= valid_max
struct Thresholds {
    double valid_min{0.0};  ///< Outside [valid_min, valid_max] the reading is physically
    double valid_max{0.0};  ///< implausible and is rejected rather than alarmed on.
    double crit_low{0.0};
    double warn_low{0.0};
    double warn_high{0.0};
    double crit_high{0.0};
    double hysteresis{0.0};       ///< An active limit clears only once back inside by this margin.
    double max_rate_per_s{0.0};   ///< |dv/dt| over DetectorConfig::rate_interval; 0 disables.
    double max_trend_per_s{0.0};  ///< |least-squares slope| over the trend window; 0 disables.

    /// Empty string if consistent, otherwise a description of the problem.
    std::string validate() const;
};

struct DetectorConfig {
    std::array<Thresholds, kSensorTypeCount> thresholds{};
    std::chrono::milliseconds stale_after{2000};
    std::size_t trend_window{20};
    std::size_t min_trend_samples{10};
    /// Rate of change is measured against the newest retained sample at least this old
    /// (or the oldest retained one). Comparing consecutive samples instead amplifies
    /// sensor noise by the sample rate and produces false alarms on fast sensors.
    std::chrono::milliseconds rate_interval{500};
    std::chrono::milliseconds max_future_skew{5000};
    std::chrono::milliseconds max_message_age{60'000};

    const Thresholds& for_type(SensorType type) const { return thresholds[index_of(type)]; }
    Thresholds& for_type(SensorType type) { return thresholds[index_of(type)]; }

    static DetectorConfig defaults();
};

/// A retained sample: sender timestamp in seconds and the value.
struct Sample {
    double t_s{0.0};
    double value{0.0};
};

/// Stateless anomaly detection rules. The detector owns no per-sensor state, has no
/// dependency on networking or threading, and reads no clock: callers pass the receiver
/// time explicitly. This keeps every rule unit-testable with plain values.
class AnomalyDetector {
public:
    explicit AnomalyDetector(DetectorConfig config) : config_(std::move(config)) {}

    const DetectorConfig& config() const { return config_; }

    /// Semantic validation of an already-parsed message against the previous accepted
    /// message from the same sensor (nullptr if none) and the receiver's wall clock.
    RejectReason validate(const TelemetryMessage& message, const TelemetryMessage* previous,
                          std::int64_t receiver_wall_us) const;

    /// Conditions implied by an accepted message. `history` holds accepted samples oldest
    /// first and must already include this message as its last element. `active` is the
    /// sensor's currently active set and is used for hysteresis.
    ConditionSet evaluate(const TelemetryMessage& message, const std::deque<Sample>& history,
                          ConditionSet active) const;

    /// Least-squares slope in value units per second, or nullopt if fewer than two
    /// samples or all samples share one timestamp.
    static std::optional<double> trend_slope(const std::deque<Sample>& samples);

private:
    DetectorConfig config_;
};

}  // namespace telemetry
