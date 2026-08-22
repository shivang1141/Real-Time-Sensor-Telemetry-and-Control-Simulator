#include "telemetry/app/telemetry_service.hpp"

#include "telemetry/protocol/protocol.hpp"

namespace telemetry {

namespace {
constexpr const char* kComponent = "service";
}

TelemetryService::TelemetryService(ServerConfig config, Logger& logger, const Clock& clock)
    : config_(std::move(config)),
      logger_(logger),
      clock_(clock),
      event_sink_(logger),
      invalid_log_limiter_(10, std::chrono::seconds(1)),
      drop_log_limiter_(1, std::chrono::seconds(1)),
      engine_(config_.processing, config_.detection, metrics_, event_sink_, clock_),
      server_(config_.network, *this, logger) {}

TelemetryService::~TelemetryService() { stop(); }

void TelemetryService::start() {
    if (running_) {
        return;
    }
    engine_.start();
    try {
        server_.start();
    } catch (...) {
        engine_.stop();
        throw;
    }
    running_ = true;
    logger_.info(kComponent, "started",
                 {{"port", server_.port()},
                  {"workers", config_.processing.workers},
                  {"queue_capacity_per_worker", config_.processing.queue_capacity_per_worker}});
}

void TelemetryService::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    logger_.info(kComponent, "stopping");
    server_.stop();
    engine_.stop();
    const auto m = metrics_.snapshot();
    logger_.info(kComponent, "stopped",
                 {{"frames_received", m.frames_received},
                  {"processed", m.messages_processed},
                  {"dropped", m.messages_dropped},
                  {"invalid", m.messages_invalid}});
}

void TelemetryService::on_connect(net::ConnectionId, const std::string&) {
    increment(metrics_.connections_accepted);
}

void TelemetryService::on_frame(net::ConnectionId id, std::string_view frame,
                                SteadyTime received_at) {
    increment(metrics_.frames_received);
    protocol::DecodeResult decoded = protocol::decode(frame);
    if (!decoded.ok()) {
        increment(metrics_.messages_invalid);
        std::uint64_t suppressed = 0;
        if (logger_.enabled(LogLevel::Warn) && invalid_log_limiter_.allow(suppressed)) {
            logger_.warn(kComponent, "invalid_message",
                         {{"conn", id},
                          {"error", protocol::to_string(decoded.error)},
                          {"detail", decoded.detail},
                          {"suppressed", suppressed}});
        }
        return;
    }
    if (engine_.submit(std::move(*decoded.message), received_at) == SubmitResult::QueueFull) {
        std::uint64_t suppressed = 0;
        if (logger_.enabled(LogLevel::Warn) && drop_log_limiter_.allow(suppressed)) {
            logger_.warn(kComponent, "queue_full_dropping",
                         {{"queue_depth", engine_.queue_depth()},
                          {"dropped_since_last_report", suppressed + 1}});
        }
    }
}

void TelemetryService::on_oversized_frame(net::ConnectionId id) {
    increment(metrics_.frames_oversized);
    std::uint64_t suppressed = 0;
    if (logger_.enabled(LogLevel::Warn) && invalid_log_limiter_.allow(suppressed)) {
        logger_.warn(kComponent, "oversized_frame",
                     {{"conn", id},
                      {"limit_bytes", config_.network.max_frame_bytes},
                      {"suppressed", suppressed}});
    }
}

void TelemetryService::on_disconnect(net::ConnectionId, net::DisconnectReason) {
    increment(metrics_.connections_closed);
}

void TelemetryService::on_connection_rejected(const std::string&) {
    increment(metrics_.connections_rejected);
}

StatusSample TelemetryService::sample(bool include_sensors) const {
    StatusSample s;
    s.at = clock_.steady_now();
    s.metrics = metrics_.snapshot();
    s.active_connections = server_.active_connections();
    s.queue_depth = engine_.queue_depth();
    s.queue_capacity = engine_.queue_capacity();
    s.queue_high_water_mark = engine_.queue_high_water_mark();
    s.workers = engine_.worker_count();
    if (include_sensors) {
        s.sensors = engine_.sensors();
        for (const SensorSnapshot& sensor : s.sensors) {
            s.health.add(sensor);
        }
    } else {
        s.health = engine_.health();
    }
    return s;
}

}  // namespace telemetry
