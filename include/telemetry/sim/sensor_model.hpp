#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include "telemetry/common/telemetry_message.hpp"

namespace telemetry::sim {

/// What happens to the sensor while it is "failed".
enum class FailMode : std::uint8_t {
    Silent,       ///< Stops sending but keeps the connection open -> server marks it stale.
    Disconnect,   ///< Closes its TCP connection and reconnects after recovery.
    FaultStatus,  ///< Keeps sending, with status=fault.
};
std::string_view to_string(FailMode mode);
std::optional<FailMode> parse_fail_mode(std::string_view text);

/// Configurable fault injection. Probabilities are per generated sample.
struct FaultSpec {
    double spike_probability{0.0};   ///< One-off value jump of +/- spike_magnitude.
    double spike_magnitude{0.0};
    double drop_probability{0.0};    ///< Sample generated (sequence advances) but never sent.
    double malformed_probability{0.0};  ///< A corrupted frame is sent instead of the sample.
    double delay_probability{0.0};   ///< Send is delayed by U[delay_min_ms, delay_max_ms].
    std::uint32_t delay_min_ms{0};
    std::uint32_t delay_max_ms{0};
    std::optional<double> ramp_after_s;  ///< From this time on, add ramp_rate_per_s * t.
    double ramp_rate_per_s{0.0};
    std::optional<double> fail_after_s;  ///< Failure window start.
    double fail_duration_s{0.0};         ///< 0 = never recovers.
    FailMode fail_mode{FailMode::Silent};
};

struct SensorSpec {
    std::string id;
    SensorType type{SensorType::Temperature};
    double rate_hz{10.0};
    double nominal{0.0};
    double noise_stddev{0.0};
    double drift_amplitude{0.0};  ///< Slow sinusoidal variation around nominal.
    double drift_period_s{60.0};
    FaultSpec faults;
};

/// Deterministic synthetic signal generator for one sensor. Given the same spec and seed
/// it produces the same sequence of actions and values (with the same standard library),
/// which makes fault scenarios reproducible. Holds no sockets or threads.
class SensorModel {
public:
    enum class Action : std::uint8_t { Send, Drop, SendMalformed };

    struct Step {
        TelemetryMessage message;
        Action action{Action::Send};
        std::chrono::milliseconds delay{0};
        bool spiked{false};
    };

    SensorModel(SensorSpec spec, std::uint64_t seed);

    /// Generates the next sample at `elapsed_s` seconds since the sensor started; advances
    /// the sequence number whatever the action.
    Step next(double elapsed_s, std::int64_t wall_us);

    /// True while inside the configured failure window.
    bool failed(double elapsed_s) const;

    /// Deterministic noise-free signal (nominal + drift + ramp) at elapsed_s.
    double baseline(double elapsed_s) const;

    /// Produces a corrupted frame for malformed-message injection.
    std::string corrupt(const std::string& valid_frame);

    const SensorSpec& spec() const { return spec_; }
    std::uint64_t next_sequence() const { return sequence_; }

private:
    bool chance(double probability);

    SensorSpec spec_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> noise_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::uint64_t sequence_{0};
};

}  // namespace telemetry::sim
