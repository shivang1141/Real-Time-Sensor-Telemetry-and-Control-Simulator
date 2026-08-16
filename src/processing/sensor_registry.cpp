#include "telemetry/processing/sensor_registry.hpp"

namespace telemetry {

std::string_view to_string(SensorHealth health) {
    switch (health) {
        case SensorHealth::Ok:
            return "ok";
        case SensorHealth::Warning:
            return "warning";
        case SensorHealth::Critical:
            return "critical";
        case SensorHealth::Fault:
            return "fault";
        case SensorHealth::Stale:
            return "stale";
    }
    return "unknown";
}

SensorHealth health_from(ConditionSet active) {
    if (has(active, Condition::Stale)) {
        return SensorHealth::Stale;
    }
    if (has(active, Condition::SensorFault)) {
        return SensorHealth::Fault;
    }
    if (has(active, Condition::CritHigh) || has(active, Condition::CritLow)) {
        return SensorHealth::Critical;
    }
    return active != 0 ? SensorHealth::Warning : SensorHealth::Ok;
}

void HealthSummary::add(const SensorSnapshot& sensor) {
    ++sensors;
    switch (sensor.health) {
        case SensorHealth::Ok:
            ++ok;
            break;
        case SensorHealth::Warning:
            ++warning;
            break;
        case SensorHealth::Critical:
            ++critical;
            break;
        case SensorHealth::Fault:
            ++fault;
            break;
        case SensorHealth::Stale:
            ++stale;
            break;
    }
    for (Condition condition : kAllConditions) {
        if (has(sensor.active, condition)) {
            if (severity_of(condition) == Severity::Critical) {
                ++active_critical_conditions;
            } else {
                ++active_warning_conditions;
            }
        }
    }
}

void HealthSummary::merge(const HealthSummary& other) {
    sensors += other.sensors;
    ok += other.ok;
    warning += other.warning;
    critical += other.critical;
    fault += other.fault;
    stale += other.stale;
    active_warning_conditions += other.active_warning_conditions;
    active_critical_conditions += other.active_critical_conditions;
}

SensorRegistry::UpdateResult SensorRegistry::update(const TelemetryMessage& message,
                                                    SteadyTime now,
                                                    std::int64_t receiver_wall_us,
                                                    std::vector<Event>& events) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = sensors_.try_emplace(message.sensor_id);
    SensorState& state = it->second;
    const std::string& id = it->first;

    if (inserted) {
        state.type = message.type;
        state.last_accepted = now;  // Staleness is measured from first contact.
        events.push_back(
            Event{EventKind::SensorRegistered, Severity::Info, id, {}, {}, 0, 0, receiver_wall_us});
    } else if (message.sequence == 0 && state.has_last && state.last.sequence != 0) {
        // Sequence numbers restart at 0 when a sensor process restarts. Treat that as a
        // new stream rather than rejecting everything as "regressed" forever.
        state.has_last = false;
        state.window.clear();
        events.push_back(
            Event{EventKind::SensorRestarted, Severity::Info, id, {}, {}, 0, 0, receiver_wall_us});
    }

    const RejectReason reason =
        detector_.validate(message, state.has_last ? &state.last : nullptr, receiver_wall_us);
    if (reason != RejectReason::None) {
        ++state.rejected;
        events.push_back(Event{EventKind::MeasurementRejected, Severity::Warning, id, {}, reason,
                               message.value, 0, receiver_wall_us});
        return UpdateResult{false, reason, 0};
    }

    std::uint64_t missing = 0;
    if (state.has_last && message.sequence > state.last.sequence + 1) {
        missing = message.sequence - state.last.sequence - 1;
        state.missing += missing;
        events.push_back(Event{EventKind::SequenceGap, Severity::Warning, id, {}, {},
                               message.value, missing, receiver_wall_us});
    }

    state.window.push_back(Sample{static_cast<double>(message.timestamp_us) / 1e6, message.value});
    while (state.window.size() > detector_.config().trend_window) {
        state.window.pop_front();
    }

    const ConditionSet before = state.active;
    // Stale is owned by check_stale(); fresh data clears it.
    const ConditionSet after =
        detector_.evaluate(message, state.window,
                           static_cast<ConditionSet>(before & ~bit(Condition::Stale)));
    emit_transitions(id, before, after, message.value, receiver_wall_us, events);

    state.active = after;
    state.last = message;
    state.has_last = true;
    state.last_accepted = now;
    ++state.accepted;
    return UpdateResult{true, RejectReason::None, missing};
}

void SensorRegistry::check_stale(SteadyTime now, std::int64_t receiver_wall_us,
                                 std::vector<Event>& events) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stale_after = detector_.config().stale_after;
    for (auto& [id, state] : sensors_) {
        if (!has(state.active, Condition::Stale) && now - state.last_accepted > stale_after) {
            state.active |= bit(Condition::Stale);
            events.push_back(Event{EventKind::ConditionRaised, Severity::Critical, id,
                                   Condition::Stale, {}, state.last.value, 0, receiver_wall_us});
        }
    }
}

void SensorRegistry::emit_transitions(const std::string& id, ConditionSet before,
                                      ConditionSet after, double value, std::int64_t wall_us,
                                      std::vector<Event>& events) {
    if (before == after) {
        return;
    }
    for (Condition condition : kAllConditions) {
        const bool was = has(before, condition);
        const bool is = has(after, condition);
        if (was == is) {
            continue;
        }
        events.push_back(Event{is ? EventKind::ConditionRaised : EventKind::ConditionCleared,
                               is ? severity_of(condition) : Severity::Info, id, condition, {},
                               value, 0, wall_us});
    }
}

SensorSnapshot SensorRegistry::make_snapshot(const std::string& id, const SensorState& state,
                                             SteadyTime now) {
    SensorSnapshot snap;
    snap.id = id;
    snap.type = state.type;
    snap.health = health_from(state.active);
    snap.active = state.active;
    snap.last_value = state.last.value;
    snap.last_sequence = state.last.sequence;
    snap.accepted = state.accepted;
    snap.rejected = state.rejected;
    snap.missing = state.missing;
    snap.since_last_accepted =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_accepted);
    return snap;
}

std::vector<SensorSnapshot> SensorRegistry::snapshot(SteadyTime now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorSnapshot> result;
    result.reserve(sensors_.size());
    for (const auto& [id, state] : sensors_) {
        result.push_back(make_snapshot(id, state, now));
    }
    return result;
}

HealthSummary SensorRegistry::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    HealthSummary summary;
    for (const auto& [id, state] : sensors_) {
        summary.add(make_snapshot(id, state, SteadyTime{}));
    }
    return summary;
}

std::size_t SensorRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensors_.size();
}

}  // namespace telemetry
