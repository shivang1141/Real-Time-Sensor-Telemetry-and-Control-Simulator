#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "telemetry/common/clock.hpp"
#include "telemetry/common/telemetry_message.hpp"
#include "telemetry/processing/anomaly_detector.hpp"
#include "telemetry/processing/events.hpp"

namespace telemetry {

enum class SensorHealth : std::uint8_t { Ok, Warning, Critical, Fault, Stale };
std::string_view to_string(SensorHealth health);

/// Health derived from active conditions. Priority: Stale > Fault > Critical > Warning.
SensorHealth health_from(ConditionSet active);

struct SensorSnapshot {
    std::string id;
    SensorType type{SensorType::Temperature};
    SensorHealth health{SensorHealth::Ok};
    ConditionSet active{0};
    double last_value{0.0};
    std::uint64_t last_sequence{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};
    std::uint64_t missing{0};
    std::chrono::milliseconds since_last_accepted{0};
};

struct HealthSummary {
    std::size_t sensors{0};
    std::size_t ok{0};
    std::size_t warning{0};
    std::size_t critical{0};
    std::size_t fault{0};
    std::size_t stale{0};
    std::size_t active_warning_conditions{0};
    std::size_t active_critical_conditions{0};

    void add(const SensorSnapshot& sensor);
    void merge(const HealthSummary& other);
};

/// State of every sensor owned by one processing shard.
///
/// Concurrency: update() and check_stale() are called only by the owning worker thread.
/// snapshot()/summary() may be called from any thread (the status reporter). A single
/// mutex guards the map; it is effectively uncontended because only the status reporter
/// competes with the worker, about once per second.
///
/// Freshness: a sensor is stale when no *accepted* message has arrived for stale_after.
/// Rejected messages do not refresh it, since they carry no usable data.
class SensorRegistry {
public:
    explicit SensorRegistry(const AnomalyDetector& detector) : detector_(detector) {}

    struct UpdateResult {
        bool accepted{false};
        RejectReason reason{RejectReason::None};
        std::uint64_t missing{0};
    };

    /// Applies one message and appends any resulting events to `events`.
    UpdateResult update(const TelemetryMessage& message, SteadyTime now,
                        std::int64_t receiver_wall_us, std::vector<Event>& events);

    /// Raises Stale for sensors with no accepted data within stale_after.
    void check_stale(SteadyTime now, std::int64_t receiver_wall_us, std::vector<Event>& events);

    std::vector<SensorSnapshot> snapshot(SteadyTime now) const;
    HealthSummary summary() const;
    std::size_t size() const;

private:
    struct SensorState {
        SensorType type{SensorType::Temperature};
        TelemetryMessage last;
        bool has_last{false};
        SteadyTime last_accepted{};
        std::deque<Sample> window;
        ConditionSet active{0};
        std::uint64_t accepted{0};
        std::uint64_t rejected{0};
        std::uint64_t missing{0};
    };

    static void emit_transitions(const std::string& id, ConditionSet before, ConditionSet after,
                                 double value, std::int64_t wall_us, std::vector<Event>& events);
    static SensorSnapshot make_snapshot(const std::string& id, const SensorState& state,
                                        SteadyTime now);

    const AnomalyDetector& detector_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SensorState> sensors_;
};

}  // namespace telemetry
