#include <gtest/gtest.h>

#include <thread>

#include "support/test_helpers.hpp"
#include "telemetry/app/telemetry_service.hpp"
#include "telemetry/sim/simulator.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

namespace {

class SimulatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ServerConfig config;
        config.network.bind_address = "127.0.0.1";
        config.network.port = 0;
        service_ = std::make_unique<TelemetryService>(config, logger_, clock_);
        service_->start();
    }

    sim::SimulatorConfig sim_config() const {
        sim::SimulatorConfig config;
        config.host = "127.0.0.1";
        config.port = service_->port();
        config.reconnect_initial = 50ms;
        config.reconnect_max = 200ms;
        return config;
    }

    static sim::SensorSpec sensor(const std::string& id, double rate_hz) {
        sim::SensorSpec spec;
        spec.id = id;
        spec.type = SensorType::Pressure;
        spec.rate_hz = rate_hz;
        spec.nominal = 101.3;
        spec.noise_stddev = 0.2;
        return spec;
    }

    Logger logger_{LogLevel::Off};
    SystemClock clock_;
    std::unique_ptr<TelemetryService> service_;
};

}  // namespace

TEST_F(SimulatorTest, StreamsTelemetryToServer) {
    auto config = sim_config();
    for (int i = 1; i <= 3; ++i) {
        config.sensors.push_back(sensor("p-00" + std::to_string(i), 100));
    }
    sim::Simulator simulator(config, logger_);
    simulator.start();
    ASSERT_TRUE(test::wait_until(
        [&] { return service_->metrics().snapshot().messages_processed >= 60; }));
    simulator.stop();
    const auto stats = simulator.stats();
    // Let the server read what is still in socket buffers before shutting it down.
    ASSERT_TRUE(test::wait_until(
        [&] { return service_->metrics().snapshot().frames_received == stats.sent; }));
    service_->stop();

    const auto m = service_->metrics().snapshot();
    EXPECT_EQ(stats.sent, m.frames_received) << "every frame sent was received";
    EXPECT_EQ(m.messages_processed, m.frames_received);
    EXPECT_EQ(m.messages_invalid, 0u);
    EXPECT_EQ(service_->sample(false).health.sensors, 3u);
}

TEST_F(SimulatorTest, DisconnectFailureReconnectsAndServerSeesGapFree) {
    auto config = sim_config();
    auto spec = sensor("p-flaky", 100);
    spec.faults.fail_after_s = 0.3;
    spec.faults.fail_duration_s = 0.3;
    spec.faults.fail_mode = sim::FailMode::Disconnect;
    config.sensors.push_back(spec);

    sim::Simulator simulator(config, logger_);
    simulator.start();
    ASSERT_TRUE(test::wait_until([&] { return simulator.stats().connects >= 2; }, 5000ms));
    ASSERT_TRUE(test::wait_until(
        [&] { return service_->metrics().snapshot().connections_closed >= 1; }));
    simulator.stop();

    const auto stats = simulator.stats();
    EXPECT_GE(stats.disconnects, 1u);
    // Samples are not generated while failed, so sequence numbers stay contiguous.
    EXPECT_TRUE(test::wait_until(
        [&] { return service_->metrics().snapshot().messages_processed == stats.sent; }));
    EXPECT_EQ(service_->metrics().snapshot().sequence_missing, 0u);
}

TEST_F(SimulatorTest, ReconnectsWhenServerStartsLate) {
    // Point the simulator at a port with no listener, then start a server there.
    ServerConfig late_config;
    late_config.network.bind_address = "127.0.0.1";
    late_config.network.port = 0;
    TelemetryService probe(late_config, logger_, clock_);
    probe.start();
    const auto port = probe.port();
    probe.stop();  // port is now free (SO_REUSEADDR lets us rebind it)

    auto config = sim_config();
    config.port = port;
    config.sensors.push_back(sensor("p-late", 50));
    sim::Simulator simulator(config, logger_);
    simulator.start();
    ASSERT_TRUE(test::wait_until([&] { return simulator.stats().connect_failures >= 1; }));

    late_config.network.port = port;
    TelemetryService late(late_config, logger_, clock_);
    late.start();
    EXPECT_TRUE(test::wait_until(
        [&] { return late.metrics().snapshot().messages_processed >= 5; }, 5000ms));
    simulator.stop();
    EXPECT_GT(simulator.stats().lost_while_disconnected, 0u);
}
