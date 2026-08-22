#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "telemetry/app/server_config.hpp"
#include "telemetry/common/clock.hpp"
#include "telemetry/common/logger.hpp"
#include "telemetry/metrics/metrics.hpp"
#include "telemetry/net/tcp_server.hpp"
#include "telemetry/processing/events.hpp"
#include "telemetry/processing/processing_engine.hpp"

namespace telemetry {

/// Everything the status reporter needs, captured at one instant.
struct StatusSample {
    SteadyTime at{};
    MetricsSnapshot metrics;
    std::size_t active_connections{0};
    std::size_t queue_depth{0};
    std::size_t queue_capacity{0};
    std::size_t queue_high_water_mark{0};
    std::size_t workers{0};
    HealthSummary health;
    std::vector<SensorSnapshot> sensors;
};

/// The central server: glues the TCP layer to the processing engine.
///
/// Data flow per frame (on the connection's thread):
///   frame -> protocol::decode -> ProcessingEngine::submit -> shard queue
/// Decoding happens on the network threads so that malformed input is rejected before
/// it consumes queue capacity, and so parsing cost scales with connections.
///
/// Shutdown order (stop()): stop the TCP server first so no new frames arrive, then
/// stop the engine, which drains the messages already queued. Nothing accepted into a
/// queue is lost on a clean shutdown.
class TelemetryService final : private net::ConnectionHandler {
public:
    TelemetryService(ServerConfig config, Logger& logger, const Clock& clock);
    ~TelemetryService() override;

    TelemetryService(const TelemetryService&) = delete;
    TelemetryService& operator=(const TelemetryService&) = delete;

    /// Starts workers, then the listener. Throws std::system_error if binding fails.
    void start();
    void stop();

    std::uint16_t port() const { return server_.port(); }
    const Metrics& metrics() const { return metrics_; }
    const ServerConfig& config() const { return config_; }
    StatusSample sample(bool include_sensors) const;

private:
    void on_connect(net::ConnectionId id, const std::string& peer) override;
    void on_frame(net::ConnectionId id, std::string_view frame, SteadyTime received_at) override;
    void on_oversized_frame(net::ConnectionId id) override;
    void on_disconnect(net::ConnectionId id, net::DisconnectReason reason) override;
    void on_connection_rejected(const std::string& peer) override;

    ServerConfig config_;
    Logger& logger_;
    const Clock& clock_;
    Metrics metrics_;
    LoggingEventSink event_sink_;
    RateLimiter invalid_log_limiter_;
    RateLimiter drop_log_limiter_;
    ProcessingEngine engine_;
    net::TcpServer server_;  // Declared last: destroyed (and stopped) before the engine.
    bool running_{false};
};

}  // namespace telemetry
