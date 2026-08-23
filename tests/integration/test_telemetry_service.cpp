#include <gtest/gtest.h>

#include <random>
#include <thread>
#include <vector>

#include "support/test_helpers.hpp"
#include "telemetry/app/telemetry_service.hpp"
#include "telemetry/net/tcp_client.hpp"
#include "telemetry/protocol/protocol.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

namespace {

class TelemetryServiceTest : public ::testing::Test {
protected:
    static ServerConfig base_config() {
        ServerConfig config;
        config.network.bind_address = "127.0.0.1";
        config.network.port = 0;
        config.processing.workers = 2;
        return config;
    }

    void start(ServerConfig config = base_config()) {
        service_ = std::make_unique<TelemetryService>(config, logger_, clock_);
        service_->start();
    }

    net::TcpClient connect() {
        net::TcpClient client;
        client.connect("127.0.0.1", service_->port());
        return client;
    }

    static std::string frame(const std::string& id, std::uint64_t seq, double value = 25.0,
                             SensorType type = SensorType::Temperature) {
        return protocol::encode(test::make_message(id, seq, value, test::now_us(), type));
    }

    MetricsSnapshot metrics() const { return service_->metrics().snapshot(); }

    bool wait_processed(std::uint64_t n, std::chrono::milliseconds timeout = 10s) {
        return test::wait_until([&] { return metrics().messages_processed >= n; }, timeout);
    }

    Logger logger_{LogLevel::Off};
    SystemClock clock_;
    std::unique_ptr<TelemetryService> service_;
};

}  // namespace

TEST_F(TelemetryServiceTest, SingleSensorEndToEnd) {
    start();
    auto client = connect();
    for (std::uint64_t i = 0; i < 50; ++i) {
        ASSERT_TRUE(client.send_line(frame("temp-001", i)));
    }
    ASSERT_TRUE(wait_processed(50));
    const auto sample = service_->sample(true);
    ASSERT_EQ(sample.sensors.size(), 1u);
    EXPECT_EQ(sample.sensors[0].id, "temp-001");
    EXPECT_EQ(sample.sensors[0].last_sequence, 49u);
    EXPECT_EQ(sample.sensors[0].health, SensorHealth::Ok);
    EXPECT_EQ(sample.metrics.sequence_missing, 0u);
    EXPECT_EQ(sample.metrics.processing_latency.count, 50u);
}

TEST_F(TelemetryServiceTest, ManySensorsSendConcurrently) {
    auto config = base_config();
    config.processing.workers = 4;
    start(config);
    constexpr int kSensors = 20;
    constexpr int kMessages = 250;
    std::vector<std::thread> threads;
    for (int s = 0; s < kSensors; ++s) {
        threads.emplace_back([&, s] {
            auto client = connect();
            const std::string id = "sensor-" + std::to_string(s);
            for (std::uint64_t i = 0; i < kMessages; ++i) {
                ASSERT_TRUE(client.send_line(frame(id, i)));
            }
        });
    }
    for (auto& t : threads) t.join();
    ASSERT_TRUE(wait_processed(kSensors * kMessages));

    const auto sample = service_->sample(true);
    EXPECT_EQ(sample.sensors.size(), static_cast<std::size_t>(kSensors));
    EXPECT_EQ(sample.metrics.messages_dropped, 0u);
    EXPECT_EQ(sample.metrics.messages_rejected, 0u);
    // Per-sensor ordering is preserved by sharding: no gaps or regressions.
    EXPECT_EQ(sample.metrics.sequence_missing, 0u);
    for (const auto& sensor : sample.sensors) {
        EXPECT_EQ(sensor.accepted, static_cast<std::uint64_t>(kMessages)) << sensor.id;
    }
}

TEST_F(TelemetryServiceTest, PartialTcpWritesAreReassembled) {
    start();
    auto client = connect();
    std::string stream;
    for (std::uint64_t i = 0; i < 20; ++i) {
        stream += frame("temp-001", i) + "\n";
    }
    // Send in random-sized chunks that split frames at arbitrary byte positions.
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> chunk(1, 37);
    for (std::size_t pos = 0; pos < stream.size();) {
        const std::size_t n = std::min(chunk(rng), stream.size() - pos);
        ASSERT_TRUE(client.send_raw(std::string_view(stream).substr(pos, n)));
        pos += n;
        std::this_thread::sleep_for(50us);
    }
    ASSERT_TRUE(wait_processed(20));
    EXPECT_EQ(metrics().messages_invalid, 0u);
    EXPECT_EQ(metrics().sequence_missing, 0u);
}

TEST_F(TelemetryServiceTest, InvalidAndOversizedMessagesAreCountedAndConnectionSurvives) {
    auto config = base_config();
    config.network.max_frame_bytes = 256;
    start(config);
    auto client = connect();
    ASSERT_TRUE(client.send_line("this is not json"));
    ASSERT_TRUE(client.send_line(R"({"id":"x","type":"temperature"})"));
    ASSERT_TRUE(client.send_line(std::string(5000, 'A')));
    ASSERT_TRUE(client.send_line(frame("temp-001", 0)));
    ASSERT_TRUE(wait_processed(1));
    const auto m = metrics();
    EXPECT_EQ(m.messages_invalid, 2u);
    EXPECT_EQ(m.frames_oversized, 1u);
    EXPECT_EQ(m.frames_received, 3u) << "oversized frames are not counted as received";
    EXPECT_EQ(m.messages_processed, 1u);
}

TEST_F(TelemetryServiceTest, SemanticallyInvalidMessagesAreRejected) {
    start();
    auto client = connect();
    ASSERT_TRUE(client.send_line(frame("temp-001", 0, 25.0)));
    ASSERT_TRUE(client.send_line(frame("temp-001", 1, 9999.0)));  // physically implausible
    ASSERT_TRUE(client.send_line(frame("temp-001", 1, 25.0)));
    ASSERT_TRUE(client.send_line(frame("temp-001", 1, 25.0)));    // duplicate sequence
    ASSERT_TRUE(wait_processed(4));
    EXPECT_EQ(metrics().messages_rejected, 2u);
}

TEST_F(TelemetryServiceTest, ClientDisconnectMidFrameIsHandled) {
    start();
    {
        auto client = connect();
        ASSERT_TRUE(client.send_line(frame("temp-001", 0)));
        const std::string half = frame("temp-001", 1);
        ASSERT_TRUE(client.send_raw(half.substr(0, half.size() / 2)));
    }
    ASSERT_TRUE(test::wait_until([&] { return metrics().connections_closed == 1; }));
    ASSERT_TRUE(wait_processed(1));
    EXPECT_EQ(metrics().frames_received, 1u);
    EXPECT_EQ(metrics().messages_invalid, 0u);

    // The server keeps serving new clients.
    auto again = connect();
    ASSERT_TRUE(again.send_line(frame("temp-001", 2)));
    ASSERT_TRUE(wait_processed(2));
}

TEST_F(TelemetryServiceTest, QueueSaturationDropsNewestAndAccountsForEveryFrame) {
    auto config = base_config();
    config.processing.workers = 1;
    config.processing.queue_capacity_per_worker = 8;
    config.processing.simulated_work = 2000us;  // ~500 msg/s capacity
    start(config);
    auto client = connect();
    constexpr std::uint64_t kFrames = 400;
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        ASSERT_TRUE(client.send_line(frame("temp-001", i)));
    }
    ASSERT_TRUE(test::wait_until([&] { return metrics().frames_received == kFrames; }));
    EXPECT_GT(metrics().messages_dropped, 0u) << "offered load exceeds capacity: drops expected";

    // Once the backlog drains, the next accepted message reveals the dropped range as a
    // sequence gap. (If the burst ends while the queue is still full, no later message
    // has been accepted yet, so the gap cannot be known.)
    ASSERT_TRUE(test::wait_until([&] { return service_->sample(false).queue_depth == 0; }));
    ASSERT_TRUE(client.send_line(frame("temp-001", kFrames)));
    ASSERT_TRUE(test::wait_until([&] { return metrics().frames_received == kFrames + 1; }));
    service_->stop();  // drains what was queued

    const auto m = metrics();
    EXPECT_EQ(m.frames_received, m.messages_invalid + m.messages_dropped + m.messages_enqueued);
    EXPECT_EQ(m.messages_processed, m.messages_enqueued) << "every queued message is processed";
    EXPECT_LE(service_->sample(false).queue_high_water_mark, 8u) << "queue stays bounded";
    EXPECT_EQ(m.sequence_missing, m.messages_dropped)
        << "every dropped message is detected downstream as a missing sequence number";
}

TEST_F(TelemetryServiceTest, GracefulShutdownDrainsQueuedMessages) {
    auto config = base_config();
    config.processing.workers = 1;
    config.processing.queue_capacity_per_worker = 1000;
    // 300 frames x 1.5 ms = 450 ms of queued work: sending finishes long before the
    // worker does, even on a heavily loaded sanitizer build.
    config.processing.simulated_work = 1500us;
    start(config);
    auto client = connect();
    constexpr std::uint64_t kFrames = 300;
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        ASSERT_TRUE(client.send_line(frame("temp-001", i)));
    }
    ASSERT_TRUE(test::wait_until([&] { return metrics().frames_received == kFrames; }));
    EXPECT_LT(metrics().messages_processed, kFrames) << "work should still be queued";

    service_->stop();
    const auto m = metrics();
    EXPECT_EQ(m.messages_dropped, 0u);
    EXPECT_EQ(m.messages_processed, kFrames) << "stop() must drain the queue";
}

TEST_F(TelemetryServiceTest, ShutdownWithIdleConnectedClientsIsPrompt) {
    start();
    std::vector<net::TcpClient> clients;
    for (int i = 0; i < 10; ++i) {
        clients.push_back(connect());
    }
    ASSERT_TRUE(test::wait_until([&] { return metrics().connections_accepted == 10; }));
    const auto begin = std::chrono::steady_clock::now();
    service_->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
    EXPECT_EQ(metrics().connections_closed, 10u);
}

TEST_F(TelemetryServiceTest, DetectsAnomaliesAndStaleSensors) {
    auto config = base_config();
    config.detection.stale_after = 300ms;
    config.processing.stale_check_interval = 50ms;
    start(config);
    auto client = connect();
    ASSERT_TRUE(client.send_line(frame("hot-001", 0, 110.0)));  // > crit_high 105
    ASSERT_TRUE(client.send_line(frame("ok-001", 0, 25.0)));
    ASSERT_TRUE(wait_processed(2));
    auto health = service_->sample(false).health;
    EXPECT_EQ(health.critical, 1u);
    EXPECT_EQ(health.ok, 1u);

    // Keep ok-001 alive; hot-001 goes silent and must become stale.
    for (std::uint64_t i = 1; i <= 8; ++i) {
        std::this_thread::sleep_for(60ms);
        ASSERT_TRUE(client.send_line(frame("ok-001", i, 25.0)));
    }
    ASSERT_TRUE(test::wait_until([&] { return service_->sample(false).health.stale == 1; }));
    const auto sensors = service_->sample(true).sensors;
    for (const auto& s : sensors) {
        if (s.id == "hot-001") {
            EXPECT_EQ(s.health, SensorHealth::Stale);
        } else {
            EXPECT_EQ(s.health, SensorHealth::Ok);
        }
    }
}
