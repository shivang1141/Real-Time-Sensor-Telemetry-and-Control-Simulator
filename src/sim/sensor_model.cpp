#include "telemetry/sim/sensor_model.hpp"

#include <cmath>

#include "telemetry/protocol/protocol.hpp"

namespace telemetry::sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

std::string_view to_string(FailMode mode) {
    switch (mode) {
        case FailMode::Silent:
            return "silent";
        case FailMode::Disconnect:
            return "disconnect";
        case FailMode::FaultStatus:
            return "fault_status";
    }
    return "unknown";
}

std::optional<FailMode> parse_fail_mode(std::string_view text) {
    for (FailMode mode : {FailMode::Silent, FailMode::Disconnect, FailMode::FaultStatus}) {
        if (to_string(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

SensorModel::SensorModel(SensorSpec spec, std::uint64_t seed)
    : spec_(std::move(spec)),
      rng_(seed),
      noise_(0.0, spec_.noise_stddev > 0 ? spec_.noise_stddev : 1.0) {}

bool SensorModel::chance(double probability) {
    return probability > 0 && unit_(rng_) < probability;
}

double SensorModel::baseline(double elapsed_s) const {
    double value = spec_.nominal;
    if (spec_.drift_amplitude != 0 && spec_.drift_period_s > 0) {
        value += spec_.drift_amplitude * std::sin(2 * kPi * elapsed_s / spec_.drift_period_s);
    }
    const auto& ramp_after = spec_.faults.ramp_after_s;
    if (ramp_after && elapsed_s > *ramp_after) {
        value += spec_.faults.ramp_rate_per_s * (elapsed_s - *ramp_after);
    }
    return value;
}

bool SensorModel::failed(double elapsed_s) const {
    const auto& start = spec_.faults.fail_after_s;
    if (!start || elapsed_s < *start) {
        return false;
    }
    return spec_.faults.fail_duration_s <= 0 || elapsed_s < *start + spec_.faults.fail_duration_s;
}

SensorModel::Step SensorModel::next(double elapsed_s, std::int64_t wall_us) {
    const FaultSpec& f = spec_.faults;
    Step step;
    step.message.sensor_id = spec_.id;
    step.message.type = spec_.type;
    step.message.timestamp_us = wall_us;
    step.message.sequence = sequence_++;

    double value = baseline(elapsed_s);
    if (spec_.noise_stddev > 0) {
        value += noise_(rng_);
    }
    if (chance(f.spike_probability)) {
        value += (unit_(rng_) < 0.5 ? -1.0 : 1.0) * f.spike_magnitude;
        step.spiked = true;
    }
    step.message.value = value;

    if (f.fail_mode == FailMode::FaultStatus && failed(elapsed_s)) {
        step.message.status = SensorStatus::Fault;
    }

    if (chance(f.drop_probability)) {
        step.action = Action::Drop;
    } else if (chance(f.malformed_probability)) {
        step.action = Action::SendMalformed;
    }
    if (chance(f.delay_probability) && f.delay_max_ms >= f.delay_min_ms) {
        std::uniform_int_distribution<std::uint32_t> delay(f.delay_min_ms, f.delay_max_ms);
        step.delay = std::chrono::milliseconds(delay(rng_));
    }
    return step;
}

std::string SensorModel::corrupt(const std::string& valid_frame) {
    switch (rng_() % 4) {
        case 0:  // Truncated JSON.
            return valid_frame.substr(0, valid_frame.size() / 2);
        case 1:  // Wrong value type.
            return R"({"id":")" + spec_.id + R"(","type":")" + std::string(to_string(spec_.type)) +
                   R"(","value":"not-a-number","ts":1,"seq":1})";
        case 2:  // Not JSON at all.
            return "garbage %%% {{{ " + std::to_string(sequence_);
        default:  // Missing required field.
            return R"({"type":")" + std::string(to_string(spec_.type)) +
                   R"(","value":1.0,"ts":1,"seq":1})";
    }
}

}  // namespace telemetry::sim
