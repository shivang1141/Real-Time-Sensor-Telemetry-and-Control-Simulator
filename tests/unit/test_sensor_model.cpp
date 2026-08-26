#include <gtest/gtest.h>

#include <cmath>

#include "telemetry/protocol/protocol.hpp"
#include "telemetry/sim/sensor_model.hpp"

using namespace telemetry;
using sim::SensorModel;
using sim::SensorSpec;

namespace {

SensorSpec basic_spec() {
    SensorSpec spec;
    spec.id = "temp-001";
    spec.type = SensorType::Temperature;
    spec.rate_hz = 10;
    spec.nominal = 25.0;
    return spec;
}

constexpr std::int64_t kWall = 1'700'000'000'000'000;

}  // namespace

TEST(SensorModel, NoiseFreeOutputEqualsNominal) {
    SensorModel model(basic_spec(), 1);
    for (int i = 0; i < 10; ++i) {
        const auto step = model.next(i * 0.1, kWall);
        EXPECT_DOUBLE_EQ(step.message.value, 25.0);
        EXPECT_EQ(step.message.sequence, static_cast<std::uint64_t>(i));
        EXPECT_EQ(step.action, SensorModel::Action::Send);
        EXPECT_EQ(step.message.sensor_id, "temp-001");
    }
}

TEST(SensorModel, SameSeedIsDeterministic) {
    auto spec = basic_spec();
    spec.noise_stddev = 1.0;
    spec.faults.spike_probability = 0.1;
    spec.faults.spike_magnitude = 50;
    spec.faults.drop_probability = 0.1;
    SensorModel a(spec, 99);
    SensorModel b(spec, 99);
    SensorModel c(spec, 100);
    bool any_difference_from_c = false;
    for (int i = 0; i < 200; ++i) {
        const auto sa = a.next(i * 0.1, kWall);
        const auto sb = b.next(i * 0.1, kWall);
        const auto sc = c.next(i * 0.1, kWall);
        EXPECT_DOUBLE_EQ(sa.message.value, sb.message.value);
        EXPECT_EQ(sa.action, sb.action);
        any_difference_from_c |= sa.message.value != sc.message.value;
    }
    EXPECT_TRUE(any_difference_from_c);
}

TEST(SensorModel, NoiseHasConfiguredSpread) {
    auto spec = basic_spec();
    spec.noise_stddev = 2.0;
    SensorModel model(spec, 5);
    double sum = 0;
    double sum_sq = 0;
    const int n = 20'000;
    for (int i = 0; i < n; ++i) {
        const double v = model.next(0.0, kWall).message.value;
        sum += v;
        sum_sq += v * v;
    }
    const double mean = sum / n;
    const double stddev = std::sqrt(sum_sq / n - mean * mean);
    EXPECT_NEAR(mean, 25.0, 0.1);
    EXPECT_NEAR(stddev, 2.0, 0.1);
}

TEST(SensorModel, RampStartsAtConfiguredTime) {
    auto spec = basic_spec();
    spec.faults.ramp_after_s = 10.0;
    spec.faults.ramp_rate_per_s = 2.0;
    SensorModel model(spec, 1);
    EXPECT_DOUBLE_EQ(model.baseline(5.0), 25.0);
    EXPECT_DOUBLE_EQ(model.baseline(10.0), 25.0);
    EXPECT_DOUBLE_EQ(model.baseline(15.0), 35.0);
}

TEST(SensorModel, FailureWindow) {
    auto spec = basic_spec();
    spec.faults.fail_after_s = 10.0;
    spec.faults.fail_duration_s = 5.0;
    SensorModel model(spec, 1);
    EXPECT_FALSE(model.failed(9.9));
    EXPECT_TRUE(model.failed(10.0));
    EXPECT_TRUE(model.failed(14.9));
    EXPECT_FALSE(model.failed(15.0));

    spec.faults.fail_duration_s = 0;  // permanent
    SensorModel permanent(spec, 1);
    EXPECT_TRUE(permanent.failed(1000.0));
}

TEST(SensorModel, FaultStatusModeSetsStatusDuringFailure) {
    auto spec = basic_spec();
    spec.faults.fail_after_s = 1.0;
    spec.faults.fail_mode = sim::FailMode::FaultStatus;
    SensorModel model(spec, 1);
    EXPECT_EQ(model.next(0.5, kWall).message.status, SensorStatus::Ok);
    EXPECT_EQ(model.next(1.5, kWall).message.status, SensorStatus::Fault);
}

TEST(SensorModel, DropAndMalformedProbabilitiesAreRespected) {
    auto spec = basic_spec();
    spec.faults.drop_probability = 0.2;
    spec.faults.malformed_probability = 0.1;
    SensorModel model(spec, 3);
    int drops = 0;
    int malformed = 0;
    const int n = 20'000;
    for (int i = 0; i < n; ++i) {
        const auto step = model.next(0.0, kWall);
        drops += step.action == SensorModel::Action::Drop;
        malformed += step.action == SensorModel::Action::SendMalformed;
    }
    EXPECT_NEAR(drops / static_cast<double>(n), 0.2, 0.02);
    // Malformed is only drawn when not dropped: 0.8 * 0.1.
    EXPECT_NEAR(malformed / static_cast<double>(n), 0.08, 0.02);
    EXPECT_EQ(model.next_sequence(), static_cast<std::uint64_t>(n))
        << "dropped samples still consume sequence numbers (that is how loss is detected)";
}

TEST(SensorModel, CorruptedFramesNeverDecode) {
    SensorModel model(basic_spec(), 11);
    const std::string valid = protocol::encode(model.next(0.0, kWall).message);
    ASSERT_TRUE(protocol::decode(valid).ok());
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(protocol::decode(model.corrupt(valid)).ok());
    }
}

TEST(SensorModel, DelayWithinConfiguredRange) {
    auto spec = basic_spec();
    spec.faults.delay_probability = 1.0;
    spec.faults.delay_min_ms = 20;
    spec.faults.delay_max_ms = 40;
    SensorModel model(spec, 1);
    for (int i = 0; i < 100; ++i) {
        const auto delay = model.next(0.0, kWall).delay.count();
        EXPECT_GE(delay, 20);
        EXPECT_LE(delay, 40);
    }
}
