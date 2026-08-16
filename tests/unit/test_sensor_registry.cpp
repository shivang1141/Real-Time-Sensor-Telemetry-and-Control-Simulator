#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "support/test_helpers.hpp"
#include "telemetry/processing/sensor_registry.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

namespace {

class SensorRegistryTest : public ::testing::Test {
protected:
    SensorRegistryTest() : detector_(make_config()), registry_(detector_) {}

    static DetectorConfig make_config() {
        DetectorConfig c = DetectorConfig::defaults();
        c.stale_after = 2000ms;
        return c;
    }

    /// Sends one temperature reading and advances the fake clock by 100 ms.
    SensorRegistry::UpdateResult send(const std::string& id, std::uint64_t seq, double value,
                                      SensorStatus status = SensorStatus::Ok) {
        auto m = test::make_message(id, seq, value, clock_.wall_now_us(),
                                    SensorType::Temperature, status);
        auto result = registry_.update(m, clock_.steady_now(), clock_.wall_now_us(), events_);
        clock_.advance(100ms);
        return result;
    }

    std::size_t count(EventKind kind, std::optional<Condition> condition = std::nullopt) const {
        return static_cast<std::size_t>(
            std::count_if(events_.begin(), events_.end(), [&](const Event& e) {
                return e.kind == kind && (!condition || e.condition == condition);
            }));
    }

    SensorSnapshot only_sensor() {
        auto all = registry_.snapshot(clock_.steady_now());
        EXPECT_EQ(all.size(), 1u);
        return all.front();
    }

    ManualClock clock_;
    AnomalyDetector detector_;
    SensorRegistry registry_;
    std::vector<Event> events_;
};

}  // namespace

TEST_F(SensorRegistryTest, FirstMessageRegistersSensor) {
    EXPECT_TRUE(send("t1", 0, 25.0).accepted);
    EXPECT_EQ(count(EventKind::SensorRegistered), 1u);
    const auto s = only_sensor();
    EXPECT_EQ(s.id, "t1");
    EXPECT_EQ(s.health, SensorHealth::Ok);
    EXPECT_EQ(s.accepted, 1u);
    EXPECT_DOUBLE_EQ(s.last_value, 25.0);
}

TEST_F(SensorRegistryTest, TracksLatestReading) {
    send("t1", 0, 25.0);
    send("t1", 1, 26.0);
    send("t1", 2, 27.5);
    const auto s = only_sensor();
    EXPECT_DOUBLE_EQ(s.last_value, 27.5);
    EXPECT_EQ(s.last_sequence, 2u);
    EXPECT_EQ(s.accepted, 3u);
}

TEST_F(SensorRegistryTest, ConditionEventsAreEdgeTriggered) {
    send("t1", 0, 25.0);
    send("t1", 1, 90.0);  // raise warn_high
    send("t1", 2, 91.0);  // still high: no new event
    send("t1", 3, 92.0);
    EXPECT_EQ(count(EventKind::ConditionRaised, Condition::WarnHigh), 1u);
    EXPECT_EQ(only_sensor().health, SensorHealth::Warning);

    send("t1", 4, 25.0);  // clear
    EXPECT_EQ(count(EventKind::ConditionCleared, Condition::WarnHigh), 1u);
    EXPECT_EQ(only_sensor().health, SensorHealth::Ok);
}

TEST_F(SensorRegistryTest, CriticalAndFaultHealth) {
    send("t1", 0, 110.0);
    EXPECT_EQ(only_sensor().health, SensorHealth::Critical);
    send("t1", 1, 25.0, SensorStatus::Fault);
    EXPECT_EQ(only_sensor().health, SensorHealth::Fault);
}

TEST_F(SensorRegistryTest, DetectsSequenceGaps) {
    send("t1", 0, 25.0);
    send("t1", 1, 25.0);
    const auto result = send("t1", 5, 25.0);  // 2, 3, 4 missing
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.missing, 3u);
    EXPECT_EQ(count(EventKind::SequenceGap), 1u);
    EXPECT_EQ(only_sensor().missing, 3u);
}

TEST_F(SensorRegistryTest, RejectsDuplicateAndOutOfRangeWithoutChangingState) {
    send("t1", 0, 25.0);
    send("t1", 1, 26.0);
    const auto dup = send("t1", 1, 99.0);
    EXPECT_FALSE(dup.accepted);
    EXPECT_EQ(dup.reason, RejectReason::SequenceRegressed);
    const auto implausible = send("t1", 2, 500.0);
    EXPECT_FALSE(implausible.accepted);
    EXPECT_EQ(implausible.reason, RejectReason::OutOfPhysicalRange);

    const auto s = only_sensor();
    EXPECT_DOUBLE_EQ(s.last_value, 26.0) << "rejected messages must not update state";
    EXPECT_EQ(s.rejected, 2u);
    EXPECT_EQ(count(EventKind::MeasurementRejected), 2u);
}

TEST_F(SensorRegistryTest, SequenceZeroAfterProgressIsTreatedAsRestart) {
    send("t1", 0, 25.0);
    send("t1", 1, 25.0);
    send("t1", 2, 25.0);
    const auto result = send("t1", 0, 25.0);
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(count(EventKind::SensorRestarted), 1u);
    EXPECT_TRUE(send("t1", 1, 25.0).accepted);
}

TEST_F(SensorRegistryTest, StaleDetectionAndRecovery) {
    send("t1", 0, 25.0);
    registry_.check_stale(clock_.steady_now(), clock_.wall_now_us(), events_);
    EXPECT_EQ(count(EventKind::ConditionRaised, Condition::Stale), 0u);

    clock_.advance(2500ms);
    registry_.check_stale(clock_.steady_now(), clock_.wall_now_us(), events_);
    EXPECT_EQ(count(EventKind::ConditionRaised, Condition::Stale), 1u);
    EXPECT_EQ(only_sensor().health, SensorHealth::Stale);

    // Repeated checks do not re-raise.
    clock_.advance(1000ms);
    registry_.check_stale(clock_.steady_now(), clock_.wall_now_us(), events_);
    EXPECT_EQ(count(EventKind::ConditionRaised, Condition::Stale), 1u);

    // Fresh data clears it.
    send("t1", 1, 25.0);
    EXPECT_EQ(count(EventKind::ConditionCleared, Condition::Stale), 1u);
    EXPECT_EQ(only_sensor().health, SensorHealth::Ok);
}

TEST_F(SensorRegistryTest, RejectedMessagesDoNotRefreshFreshness) {
    send("t1", 0, 25.0);
    clock_.advance(1500ms);
    send("t1", 1, 999.0);  // rejected
    clock_.advance(1000ms);
    registry_.check_stale(clock_.steady_now(), clock_.wall_now_us(), events_);
    EXPECT_EQ(only_sensor().health, SensorHealth::Stale);
}

TEST_F(SensorRegistryTest, SummaryCountsHealthAndConditions) {
    send("a", 0, 25.0);
    send("b", 0, 90.0);   // warning
    send("c", 0, 110.0);  // critical (warn_high + crit_high)
    const auto summary = registry_.summary();
    EXPECT_EQ(summary.sensors, 3u);
    EXPECT_EQ(summary.ok, 1u);
    EXPECT_EQ(summary.warning, 1u);
    EXPECT_EQ(summary.critical, 1u);
    EXPECT_EQ(summary.active_warning_conditions, 2u);
    EXPECT_EQ(summary.active_critical_conditions, 1u);
}

TEST(SensorHealth, PriorityOrder) {
    EXPECT_EQ(health_from(0), SensorHealth::Ok);
    EXPECT_EQ(health_from(bit(Condition::Trend)), SensorHealth::Warning);
    EXPECT_EQ(health_from(bit(Condition::Trend) | bit(Condition::CritHigh)),
              SensorHealth::Critical);
    EXPECT_EQ(health_from(bit(Condition::CritHigh) | bit(Condition::SensorFault)),
              SensorHealth::Fault);
    EXPECT_EQ(health_from(bit(Condition::SensorFault) | bit(Condition::Stale)),
              SensorHealth::Stale);
}
