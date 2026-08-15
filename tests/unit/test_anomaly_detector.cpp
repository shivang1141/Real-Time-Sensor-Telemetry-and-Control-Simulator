#include <gtest/gtest.h>

#include <deque>

#include "support/test_helpers.hpp"
#include "telemetry/processing/anomaly_detector.hpp"

using namespace telemetry;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000;  // Receiver wall clock (us).

DetectorConfig test_config() {
    DetectorConfig c = DetectorConfig::defaults();
    // Temperature: valid [-60,200], crit [-20,105], warn [0,85], hysteresis 1.
    c.for_type(SensorType::Temperature) = {-60, 200, -20, 0, 85, 105, 1.0, 20, 2.0};
    c.trend_window = 20;
    c.min_trend_samples = 10;
    c.rate_interval = std::chrono::milliseconds(500);
    return c;
}

TelemetryMessage temp(double value, std::uint64_t seq = 1, std::int64_t ts = kNow,
                      SensorStatus status = SensorStatus::Ok) {
    return test::make_message("t1", seq, value, ts, SensorType::Temperature, status);
}

/// Evaluates a single sample with no history.
ConditionSet eval(const AnomalyDetector& d, double value, ConditionSet active = 0) {
    const auto m = temp(value);
    const std::deque<Sample> history{{static_cast<double>(m.timestamp_us) / 1e6, value}};
    return d.evaluate(m, history, active);
}

}  // namespace

TEST(Thresholds, DefaultsAreConsistent) {
    const auto config = DetectorConfig::defaults();
    for (SensorType type : kAllSensorTypes) {
        EXPECT_EQ(config.for_type(type).validate(), "") << to_string(type);
    }
}

TEST(Thresholds, ValidateDetectsMisorderedLimits) {
    Thresholds t{0, 100, 10, 20, 15, 90, 0, 0, 0};  // warn_low > warn_high
    EXPECT_NE(t.validate(), "");
    Thresholds negative{0, 100, 10, 20, 80, 90, -1, 0, 0};
    EXPECT_NE(negative.validate(), "");
}

TEST(AnomalyDetector, NormalValueRaisesNothing) {
    AnomalyDetector d(test_config());
    EXPECT_EQ(eval(d, 25.0), 0);
}

TEST(AnomalyDetector, WarningAndCriticalLimits) {
    AnomalyDetector d(test_config());
    EXPECT_EQ(eval(d, 90.0), bit(Condition::WarnHigh));
    EXPECT_EQ(eval(d, 110.0), bit(Condition::WarnHigh) | bit(Condition::CritHigh));
    EXPECT_EQ(eval(d, -5.0), bit(Condition::WarnLow));
    EXPECT_EQ(eval(d, -30.0), bit(Condition::WarnLow) | bit(Condition::CritLow));
}

TEST(AnomalyDetector, LimitBoundariesAreInclusiveOfNormal) {
    AnomalyDetector d(test_config());
    EXPECT_EQ(eval(d, 85.0), 0) << "exactly at warn_high is not a warning";
    EXPECT_EQ(eval(d, 0.0), 0) << "exactly at warn_low is not a warning";
}

TEST(AnomalyDetector, HysteresisPreventsFlapping) {
    AnomalyDetector d(test_config());
    const ConditionSet active = bit(Condition::WarnHigh);
    // Once raised at >85, it stays raised until the value drops to <=84 (hysteresis 1).
    EXPECT_EQ(eval(d, 84.5, active), bit(Condition::WarnHigh));
    EXPECT_EQ(eval(d, 84.0, active), 0);
    // Not active: 84.5 does not raise.
    EXPECT_EQ(eval(d, 84.5, 0), 0);
    // Low side mirrors it.
    EXPECT_EQ(eval(d, 0.5, bit(Condition::WarnLow)), bit(Condition::WarnLow));
    EXPECT_EQ(eval(d, 1.0, bit(Condition::WarnLow)), 0);
}

TEST(AnomalyDetector, SensorStatusConditions) {
    AnomalyDetector d(test_config());
    const auto degraded = temp(25.0, 1, kNow, SensorStatus::Degraded);
    const auto fault = temp(25.0, 1, kNow, SensorStatus::Fault);
    const std::deque<Sample> h{{1.0, 25.0}};
    EXPECT_EQ(d.evaluate(degraded, h, 0), bit(Condition::SensorDegraded));
    EXPECT_EQ(d.evaluate(fault, h, 0), bit(Condition::SensorFault));
}

TEST(AnomalyDetector, RateOfChangeUsesReferenceAtLeastRateIntervalOld) {
    AnomalyDetector d(test_config());  // max 20 units/s over >= 0.5 s.
    const auto m = temp(30.0);
    // Samples every 0.1 s. Small noisy steps between neighbours (2 units in 0.1 s would
    // be 20/s and trip a consecutive-sample check) must not raise.
    std::deque<Sample> noisy{{0.0, 25}, {0.1, 27}, {0.2, 25}, {0.3, 27}, {0.4, 25}, {0.5, 27},
                             {0.6, 25}};
    EXPECT_FALSE(has(d.evaluate(m, noisy, 0), Condition::RateOfChange));

    // A genuine jump of 30 units within 0.5 s (60/s) raises.
    std::deque<Sample> jump{{0.0, 25}, {0.1, 25}, {0.2, 25}, {0.3, 25}, {0.4, 25}, {0.5, 25},
                            {0.6, 55}};
    EXPECT_TRUE(has(d.evaluate(m, jump, 0), Condition::RateOfChange));
}

TEST(AnomalyDetector, RateOfChangeFallsBackToOldestSample) {
    AnomalyDetector d(test_config());
    const auto m = temp(30.0);
    // History spans only 0.2 s: the oldest sample is used as reference.
    std::deque<Sample> short_history{{0.0, 25}, {0.1, 25}, {0.2, 40}};  // 75/s
    EXPECT_TRUE(has(d.evaluate(m, short_history, 0), Condition::RateOfChange));
}

TEST(AnomalyDetector, TrendRequiresMinimumSamples) {
    AnomalyDetector d(test_config());  // max trend 2 units/s, min 10 samples.
    const auto m = temp(30.0);
    std::deque<Sample> rising;
    for (int i = 0; i < 9; ++i) {
        rising.push_back({i * 1.0, 25.0 + 3.0 * i});  // 3 units/s
    }
    EXPECT_FALSE(has(d.evaluate(m, rising, 0), Condition::Trend)) << "only 9 samples";
    rising.push_back({9.0, 52.0});
    EXPECT_TRUE(has(d.evaluate(m, rising, 0), Condition::Trend));
}

TEST(AnomalyDetector, TrendSlopeIsLeastSquares) {
    std::deque<Sample> line;
    const double t0 = 1.7e9;  // Realistic epoch seconds: checks numerical stability.
    for (int i = 0; i < 20; ++i) {
        line.push_back({t0 + i * 0.1, 10.0 + 4.0 * (i * 0.1)});
    }
    const auto slope = AnomalyDetector::trend_slope(line);
    ASSERT_TRUE(slope.has_value());
    EXPECT_NEAR(*slope, 4.0, 1e-6);

    EXPECT_FALSE(AnomalyDetector::trend_slope({{1.0, 2.0}}).has_value());
    EXPECT_FALSE(AnomalyDetector::trend_slope({{1.0, 2.0}, {1.0, 3.0}}).has_value());
}

TEST(AnomalyDetector, ValidateAcceptsNormalMessage) {
    AnomalyDetector d(test_config());
    EXPECT_EQ(d.validate(temp(25.0), nullptr, kNow), RejectReason::None);
}

TEST(AnomalyDetector, ValidateRejectsPhysicallyImplausibleValues) {
    AnomalyDetector d(test_config());
    EXPECT_EQ(d.validate(temp(250.0), nullptr, kNow), RejectReason::OutOfPhysicalRange);
    EXPECT_EQ(d.validate(temp(-61.0), nullptr, kNow), RejectReason::OutOfPhysicalRange);
    EXPECT_EQ(d.validate(temp(200.0), nullptr, kNow), RejectReason::None);
}

TEST(AnomalyDetector, ValidateRejectsBadTimestamps) {
    AnomalyDetector d(test_config());  // max skew 5 s, max age 60 s.
    EXPECT_EQ(d.validate(temp(25, 1, kNow + 6'000'000), nullptr, kNow),
              RejectReason::TimestampInFuture);
    EXPECT_EQ(d.validate(temp(25, 1, kNow + 4'000'000), nullptr, kNow), RejectReason::None);
    EXPECT_EQ(d.validate(temp(25, 1, kNow - 61'000'000), nullptr, kNow),
              RejectReason::TimestampTooOld);
}

TEST(AnomalyDetector, ValidateAgainstPreviousMessage) {
    AnomalyDetector d(test_config());
    const auto previous = temp(25.0, 10, kNow - 1000);
    EXPECT_EQ(d.validate(temp(25, 11, kNow), &previous, kNow), RejectReason::None);
    EXPECT_EQ(d.validate(temp(25, 10, kNow), &previous, kNow), RejectReason::SequenceRegressed);
    EXPECT_EQ(d.validate(temp(25, 9, kNow), &previous, kNow), RejectReason::SequenceRegressed);
    EXPECT_EQ(d.validate(temp(25, 11, kNow - 2000), &previous, kNow),
              RejectReason::TimestampRegressed);

    auto other_type = test::make_message("t1", 11, 100.0, kNow, SensorType::Pressure);
    EXPECT_EQ(d.validate(other_type, &previous, kNow), RejectReason::TypeMismatch);
}
