#include "telemetry/app/status_reporter.hpp"

#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <sstream>

namespace telemetry {

namespace {

std::string fixed(double value, int precision) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", precision, value);
    return buffer;
}

std::string conditions_text(ConditionSet active) {
    std::string text;
    for (Condition condition : kAllConditions) {
        if (has(active, condition)) {
            if (!text.empty()) {
                text += ',';
            }
            text += to_string(condition);
        }
    }
    return text.empty() ? "-" : text;
}

nlohmann::json histogram_json(const HistogramSnapshot& h) {
    return {{"count", h.count},
            {"mean_us", h.mean()},
            {"p50_us", h.percentile(0.50)},
            {"p95_us", h.percentile(0.95)},
            {"p99_us", h.percentile(0.99)},
            {"max_us", h.max_us}};
}

}  // namespace

IntervalStats compute_interval(const StatusSample& previous, const StatusSample& current) {
    IntervalStats stats;
    stats.seconds = std::chrono::duration<double>(current.at - previous.at).count();
    const auto& p = previous.metrics;
    const auto& c = current.metrics;
    if (stats.seconds > 0) {
        stats.frames_per_s =
            static_cast<double>(c.frames_received - p.frames_received) / stats.seconds;
        stats.processed_per_s =
            static_cast<double>(c.messages_processed - p.messages_processed) / stats.seconds;
        stats.dropped_per_s =
            static_cast<double>(c.messages_dropped - p.messages_dropped) / stats.seconds;
    }
    stats.processing_latency = c.processing_latency.since(p.processing_latency);
    return stats;
}

std::string format_status(const StatusSample& s, const IntervalStats& interval, double uptime_s,
                          std::size_t max_listed_sensors) {
    const auto& m = s.metrics;
    const auto& lat = interval.processing_latency;
    std::ostringstream out;
    out << "---- status t=" << fixed(uptime_s, 1) << "s ----\n";
    out << "  connections   active=" << s.active_connections << " accepted="
        << m.connections_accepted << " rejected=" << m.connections_rejected << "\n";
    out << "  messages      recv=" << m.frames_received << " (" << fixed(interval.frames_per_s, 0)
        << "/s) processed=" << m.messages_processed << " ("
        << fixed(interval.processed_per_s, 0) << "/s)\n";
    out << "  errors        invalid=" << m.messages_invalid << " oversized=" << m.frames_oversized
        << " rejected=" << m.messages_rejected << " dropped=" << m.messages_dropped
        << " seq_missing=" << m.sequence_missing << "\n";
    out << "  queue         depth=" << s.queue_depth << "/" << s.queue_capacity
        << " peak_shard=" << s.queue_high_water_mark << " workers=" << s.workers << "\n";
    out << "  latency(us)   interval n=" << lat.count << " mean=" << fixed(lat.mean(), 1)
        << " p50=" << lat.percentile(0.50) << " p95=" << lat.percentile(0.95)
        << " p99=" << lat.percentile(0.99) << " max=" << lat.max_us << "\n";
    out << "  sensors       total=" << s.health.sensors << " ok=" << s.health.ok
        << " warning=" << s.health.warning << " critical=" << s.health.critical
        << " fault=" << s.health.fault << " stale=" << s.health.stale
        << " | active warnings=" << s.health.active_warning_conditions
        << " critical=" << s.health.active_critical_conditions << "\n";

    std::size_t listed = 0;
    for (const SensorSnapshot& sensor : s.sensors) {
        if (sensor.health == SensorHealth::Ok) {
            continue;
        }
        if (listed == max_listed_sensors) {
            out << "    ... more sensors not shown\n";
            break;
        }
        ++listed;
        char line[256];
        std::snprintf(line, sizeof line, "    %-20s %-16s %-9s value=%-10.3f age=%lldms [%s]\n",
                      sensor.id.c_str(), std::string(to_string(sensor.type)).c_str(),
                      std::string(to_string(sensor.health)).c_str(), sensor.last_value,
                      static_cast<long long>(sensor.since_last_accepted.count()),
                      conditions_text(sensor.active).c_str());
        out << line;
    }
    return out.str();
}

std::string format_final_summary(const StatusSample& s, double uptime_s) {
    const auto& m = s.metrics;
    std::ostringstream out;
    out << "==== final summary (uptime " << fixed(uptime_s, 1) << "s) ====\n"
        << "  frames received     " << m.frames_received << "\n"
        << "  processed           " << m.messages_processed << "\n"
        << "  invalid             " << m.messages_invalid << "\n"
        << "  oversized           " << m.frames_oversized << "\n"
        << "  rejected            " << m.messages_rejected << "\n"
        << "  dropped (queue)     " << m.messages_dropped << "\n"
        << "  sequence missing    " << m.sequence_missing << "\n"
        << "  peak shard queue    " << s.queue_high_water_mark << "\n"
        << "  processing latency  mean=" << fixed(m.processing_latency.mean(), 1)
        << "us p50=" << m.processing_latency.percentile(0.5)
        << "us p95=" << m.processing_latency.percentile(0.95)
        << "us p99=" << m.processing_latency.percentile(0.99)
        << "us max=" << m.processing_latency.max_us << "us\n"
        << "  sensors             " << s.health.sensors << " (ok=" << s.health.ok
        << " warning=" << s.health.warning << " critical=" << s.health.critical
        << " fault=" << s.health.fault << " stale=" << s.health.stale << ")\n";
    return out.str();
}

std::string metrics_to_json(const StatusSample& s, double uptime_s) {
    const auto& m = s.metrics;
    const nlohmann::json root = {
        {"uptime_s", uptime_s},
        {"workers", s.workers},
        {"connections", {{"accepted", m.connections_accepted},
                         {"rejected", m.connections_rejected},
                         {"closed", m.connections_closed}}},
        {"messages", {{"frames_received", m.frames_received},
                      {"oversized", m.frames_oversized},
                      {"invalid", m.messages_invalid},
                      {"enqueued", m.messages_enqueued},
                      {"dropped", m.messages_dropped},
                      {"processed", m.messages_processed},
                      {"rejected", m.messages_rejected},
                      {"sequence_missing", m.sequence_missing}}},
        {"queue", {{"capacity", s.queue_capacity},
                   {"peak_shard_depth", s.queue_high_water_mark}}},
        {"processing_latency", histogram_json(m.processing_latency)},
        {"queue_wait", histogram_json(m.queue_wait)},
        {"end_to_end_latency", histogram_json(m.end_to_end_latency)},
        {"sensors", {{"total", s.health.sensors},
                     {"ok", s.health.ok},
                     {"warning", s.health.warning},
                     {"critical", s.health.critical},
                     {"fault", s.health.fault},
                     {"stale", s.health.stale}}},
    };
    return root.dump(2);
}

}  // namespace telemetry
