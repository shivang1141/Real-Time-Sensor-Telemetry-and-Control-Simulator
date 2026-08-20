#include "telemetry/app/server_config.hpp"

#include "common/json_reader.hpp"

namespace telemetry {

namespace {

using detail::ObjectReader;
using nlohmann::json;
using std::chrono::microseconds;
using std::chrono::milliseconds;

void read_network(const json& node, net::TcpServerConfig& out) {
    ObjectReader r(node, "network");
    out.bind_address = r.get<std::string>("bind_address", out.bind_address);
    out.port = r.get<std::uint16_t>("port", out.port);
    out.backlog = r.get<int>("backlog", out.backlog);
    out.max_connections = r.get<std::size_t>("max_connections", out.max_connections);
    out.max_frame_bytes = r.get<std::size_t>("max_frame_bytes", out.max_frame_bytes);
    out.recv_buffer_bytes = r.get<std::size_t>("recv_buffer_bytes", out.recv_buffer_bytes);
    out.idle_timeout =
        milliseconds(r.get<std::uint32_t>("idle_timeout_ms",
                                          static_cast<std::uint32_t>(out.idle_timeout.count())));
    r.finish();
}

void read_processing(const json& node, ProcessingConfig& out) {
    ObjectReader r(node, "processing");
    out.workers = r.get<std::size_t>("workers", out.workers);
    out.queue_capacity_per_worker =
        r.get<std::size_t>("queue_capacity_per_worker", out.queue_capacity_per_worker);
    out.stale_check_interval = milliseconds(r.get<std::uint32_t>(
        "stale_check_interval_ms", static_cast<std::uint32_t>(out.stale_check_interval.count())));
    out.simulated_work = microseconds(r.get<std::uint32_t>(
        "simulated_work_us", static_cast<std::uint32_t>(out.simulated_work.count())));
    r.finish();
}

void read_thresholds(const json& node, const std::string& path, Thresholds& out) {
    ObjectReader r(node, path);
    out.valid_min = r.get<double>("valid_min", out.valid_min);
    out.valid_max = r.get<double>("valid_max", out.valid_max);
    out.crit_low = r.get<double>("crit_low", out.crit_low);
    out.warn_low = r.get<double>("warn_low", out.warn_low);
    out.warn_high = r.get<double>("warn_high", out.warn_high);
    out.crit_high = r.get<double>("crit_high", out.crit_high);
    out.hysteresis = r.get<double>("hysteresis", out.hysteresis);
    out.max_rate_per_s = r.get<double>("max_rate_per_s", out.max_rate_per_s);
    out.max_trend_per_s = r.get<double>("max_trend_per_s", out.max_trend_per_s);
    r.finish();
}

void read_detection(const json& node, DetectorConfig& out) {
    ObjectReader r(node, "detection");
    out.stale_after = milliseconds(
        r.get<std::uint32_t>("stale_after_ms", static_cast<std::uint32_t>(out.stale_after.count())));
    out.trend_window = r.get<std::size_t>("trend_window", out.trend_window);
    out.min_trend_samples = r.get<std::size_t>("min_trend_samples", out.min_trend_samples);
    out.rate_interval = milliseconds(r.get<std::uint32_t>(
        "rate_interval_ms", static_cast<std::uint32_t>(out.rate_interval.count())));
    out.max_future_skew = milliseconds(r.get<std::uint32_t>(
        "max_future_skew_ms", static_cast<std::uint32_t>(out.max_future_skew.count())));
    out.max_message_age = milliseconds(r.get<std::uint32_t>(
        "max_message_age_ms", static_cast<std::uint32_t>(out.max_message_age.count())));
    if (const json* thresholds = r.child("thresholds")) {
        ObjectReader tr(*thresholds, "detection.thresholds");
        for (SensorType type : kAllSensorTypes) {
            const std::string name(to_string(type));
            if (const json* t = tr.child(name.c_str())) {
                read_thresholds(*t, tr.child_path(name), out.for_type(type));
            }
        }
        tr.finish();
    }
    r.finish();
}

void read_logging(const json& node, LoggingConfig& out) {
    ObjectReader r(node, "logging");
    if (const auto level = r.get_optional<std::string>("level")) {
        const auto parsed = parse_log_level(*level);
        if (!parsed) {
            throw ConfigError("logging.level: expected debug|info|warn|error|off");
        }
        out.level = *parsed;
    }
    out.file = r.get<std::string>("file", out.file);
    r.finish();
}

void read_status(const json& node, StatusConfig& out) {
    ObjectReader r(node, "status");
    out.interval = milliseconds(
        r.get<std::uint32_t>("interval_ms", static_cast<std::uint32_t>(out.interval.count())));
    out.max_listed_sensors = r.get<std::size_t>("max_listed_sensors", out.max_listed_sensors);
    r.finish();
}

ServerConfig from_json(const json& root) {
    ServerConfig config;
    ObjectReader r(root, "");
    if (const json* n = r.child("network")) read_network(*n, config.network);
    if (const json* n = r.child("processing")) read_processing(*n, config.processing);
    if (const json* n = r.child("detection")) read_detection(*n, config.detection);
    if (const json* n = r.child("logging")) read_logging(*n, config.logging);
    if (const json* n = r.child("status")) read_status(*n, config.status);
    r.finish();
    config.validate();
    return config;
}

}  // namespace

void ServerConfig::validate() const {
    if (processing.workers == 0 || processing.workers > 256) {
        throw ConfigError("processing.workers must be in [1, 256]");
    }
    if (processing.queue_capacity_per_worker == 0) {
        throw ConfigError("processing.queue_capacity_per_worker must be > 0");
    }
    if (processing.stale_check_interval.count() == 0) {
        throw ConfigError("processing.stale_check_interval_ms must be > 0");
    }
    if (network.max_connections == 0) {
        throw ConfigError("network.max_connections must be > 0");
    }
    if (network.max_frame_bytes < 64 || network.max_frame_bytes > 1024 * 1024) {
        throw ConfigError("network.max_frame_bytes must be in [64, 1048576]");
    }
    if (network.recv_buffer_bytes < 512) {
        throw ConfigError("network.recv_buffer_bytes must be >= 512");
    }
    if (network.backlog <= 0) {
        throw ConfigError("network.backlog must be > 0");
    }
    if (detection.trend_window < 2 || detection.min_trend_samples < 2 ||
        detection.min_trend_samples > detection.trend_window) {
        throw ConfigError(
            "detection: require 2 <= min_trend_samples <= trend_window");
    }
    if (detection.stale_after.count() == 0) {
        throw ConfigError("detection.stale_after_ms must be > 0");
    }
    for (SensorType type : kAllSensorTypes) {
        const std::string problem = detection.for_type(type).validate();
        if (!problem.empty()) {
            throw ConfigError("detection.thresholds." + std::string(to_string(type)) + ": " +
                              problem);
        }
    }
}

ServerConfig load_server_config(const std::string& path) {
    return from_json(detail::load_json_file(path));
}

ServerConfig parse_server_config(const std::string& json_text) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false,
                                  /*ignore_comments=*/true);
    if (root.is_discarded()) {
        throw ConfigError("config is not valid JSON");
    }
    return from_json(root);
}

}  // namespace telemetry
