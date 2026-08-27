#include <cstdio>

#include "common/json_reader.hpp"
#include "telemetry/protocol/protocol.hpp"
#include "telemetry/sim/simulator.hpp"

namespace telemetry::sim {

namespace {

using detail::ObjectReader;
using nlohmann::json;
using std::chrono::milliseconds;

void check_probability(double p, const std::string& path) {
    if (p < 0.0 || p > 1.0) {
        throw ConfigError(path + ": probability must be in [0, 1]");
    }
}

FaultSpec read_faults(const json& node, const std::string& path) {
    ObjectReader r(node, path);
    FaultSpec f;
    f.spike_probability = r.get<double>("spike_probability", 0.0);
    f.spike_magnitude = r.get<double>("spike_magnitude", 0.0);
    f.drop_probability = r.get<double>("drop_probability", 0.0);
    f.malformed_probability = r.get<double>("malformed_probability", 0.0);
    f.delay_probability = r.get<double>("delay_probability", 0.0);
    f.delay_min_ms = r.get<std::uint32_t>("delay_min_ms", 0);
    f.delay_max_ms = r.get<std::uint32_t>("delay_max_ms", 0);
    f.ramp_after_s = r.get_optional<double>("ramp_after_s");
    f.ramp_rate_per_s = r.get<double>("ramp_rate_per_s", 0.0);
    f.fail_after_s = r.get_optional<double>("fail_after_s");
    f.fail_duration_s = r.get<double>("fail_duration_s", 0.0);
    if (const auto mode = r.get_optional<std::string>("fail_mode")) {
        const auto parsed = parse_fail_mode(*mode);
        if (!parsed) {
            throw ConfigError(path + ".fail_mode: expected silent|disconnect|fault_status");
        }
        f.fail_mode = *parsed;
    }
    r.finish();

    check_probability(f.spike_probability, path + ".spike_probability");
    check_probability(f.drop_probability, path + ".drop_probability");
    check_probability(f.malformed_probability, path + ".malformed_probability");
    check_probability(f.delay_probability, path + ".delay_probability");
    if (f.delay_max_ms < f.delay_min_ms) {
        throw ConfigError(path + ": delay_max_ms must be >= delay_min_ms");
    }
    return f;
}

void read_group(const json& node, const std::string& path, std::vector<SensorSpec>& out) {
    ObjectReader r(node, path);
    const auto prefix = r.require<std::string>("id_prefix");
    const auto type_name = r.require<std::string>("type");
    const auto type = parse_sensor_type(type_name);
    if (!type) {
        throw ConfigError(path + ".type: unknown sensor type '" + type_name + "'");
    }
    const auto count = r.get<std::uint32_t>("count", 1);
    SensorSpec base;
    base.type = *type;
    base.rate_hz = r.get<double>("rate_hz", 10.0);
    base.nominal = r.require<double>("nominal");
    base.noise_stddev = r.get<double>("noise_stddev", 0.0);
    base.drift_amplitude = r.get<double>("drift_amplitude", 0.0);
    base.drift_period_s = r.get<double>("drift_period_s", 60.0);
    if (const json* faults = r.child("faults")) {
        base.faults = read_faults(*faults, r.child_path("faults"));
    }
    r.finish();

    if (count == 0 || count > 10000) {
        throw ConfigError(path + ".count must be in [1, 10000]");
    }
    if (base.rate_hz <= 0 || base.rate_hz > 100000) {
        throw ConfigError(path + ".rate_hz must be in (0, 100000]");
    }
    if (base.noise_stddev < 0) {
        throw ConfigError(path + ".noise_stddev must be >= 0");
    }
    for (std::uint32_t i = 1; i <= count; ++i) {
        SensorSpec spec = base;
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "-%03u", i);
        spec.id = prefix + suffix;
        if (!protocol::is_valid_sensor_id(spec.id)) {
            throw ConfigError(path + ".id_prefix produces invalid sensor id '" + spec.id + "'");
        }
        out.push_back(std::move(spec));
    }
}

SimulatorConfig from_json(const json& root) {
    SimulatorConfig config;
    ObjectReader r(root, "");
    if (const json* server = r.child("server")) {
        ObjectReader sr(*server, "server");
        config.host = sr.get<std::string>("host", config.host);
        config.port = sr.get<std::uint16_t>("port", config.port);
        sr.finish();
    }
    config.seed = r.get<std::uint64_t>("seed", config.seed);
    config.duration_s = r.get<double>("duration_s", config.duration_s);
    config.connect_timeout = milliseconds(r.get<std::uint32_t>("connect_timeout_ms", 1000));
    config.reconnect_initial = milliseconds(r.get<std::uint32_t>("reconnect_initial_ms", 200));
    config.reconnect_max = milliseconds(r.get<std::uint32_t>("reconnect_max_ms", 5000));
    config.stats_interval = milliseconds(r.get<std::uint32_t>("stats_interval_ms", 5000));

    const json* groups = r.child("groups");
    if (groups == nullptr || !groups->is_array() || groups->empty()) {
        throw ConfigError("groups: expected a non-empty array of sensor groups");
    }
    for (std::size_t i = 0; i < groups->size(); ++i) {
        read_group((*groups)[i], "groups[" + std::to_string(i) + "]", config.sensors);
    }
    r.finish();

    if (config.duration_s < 0) {
        throw ConfigError("duration_s must be >= 0");
    }
    if (config.reconnect_initial.count() == 0 || config.reconnect_max < config.reconnect_initial) {
        throw ConfigError("require 0 < reconnect_initial_ms <= reconnect_max_ms");
    }
    return config;
}

}  // namespace

SimulatorConfig load_simulator_config(const std::string& path) {
    return from_json(detail::load_json_file(path));
}

SimulatorConfig parse_simulator_config(const std::string& json_text) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false,
                                  /*ignore_comments=*/true);
    if (root.is_discarded()) {
        throw ConfigError("config is not valid JSON");
    }
    return from_json(root);
}

}  // namespace telemetry::sim
