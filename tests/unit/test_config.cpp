#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "telemetry/app/server_config.hpp"
#include "telemetry/sim/simulator.hpp"

using namespace telemetry;

#ifndef TELEMETRY_SOURCE_DIR
#define TELEMETRY_SOURCE_DIR "."
#endif

namespace {

std::string config_path(const std::string& relative) {
    return std::string(TELEMETRY_SOURCE_DIR) + "/config/" + relative;
}

void expect_config_error(const std::string& json, const std::string& expected_substring) {
    try {
        parse_server_config(json);
        FAIL() << "expected ConfigError for " << json;
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find(expected_substring), std::string::npos)
            << "message was: " << e.what();
    }
}

}  // namespace

TEST(ServerConfig, EmptyObjectGivesValidDefaults) {
    const auto config = parse_server_config("{}");
    EXPECT_EQ(config.network.port, 9000);
    EXPECT_EQ(config.processing.workers, 2u);
    EXPECT_NO_THROW(config.validate());
}

TEST(ServerConfig, OverridesAndPartialThresholds) {
    const auto config = parse_server_config(R"({
        // comments are allowed
        "network": {"port": 1234, "max_frame_bytes": 2048},
        "processing": {"workers": 8},
        "detection": {"stale_after_ms": 500,
                      "thresholds": {"temperature": {"warn_high": 70}}},
        "logging": {"level": "debug"}
    })");
    EXPECT_EQ(config.network.port, 1234);
    EXPECT_EQ(config.network.max_frame_bytes, 2048u);
    EXPECT_EQ(config.processing.workers, 8u);
    EXPECT_EQ(config.detection.stale_after.count(), 500);
    EXPECT_EQ(config.detection.for_type(SensorType::Temperature).warn_high, 70);
    // Untouched threshold fields keep their defaults.
    EXPECT_EQ(config.detection.for_type(SensorType::Temperature).crit_high, 105);
    EXPECT_EQ(config.logging.level, LogLevel::Debug);
}

TEST(ServerConfig, RejectsUnknownFields) {
    expect_config_error(R"({"netwrok": {}})", "netwrok: unknown field");
    expect_config_error(R"({"network": {"prot": 1}})", "network.prot: unknown field");
    expect_config_error(R"({"detection": {"thresholds": {"humidity": {}}}})",
                        "detection.thresholds.humidity");
}

TEST(ServerConfig, RejectsWrongTypesAndRanges) {
    expect_config_error(R"({"network": {"port": "9000"}})", "network.port: expected unsigned");
    expect_config_error(R"({"network": {"port": 70000}})", "network.port: value too large");
    expect_config_error(R"({"processing": {"workers": 0}})", "processing.workers");
    expect_config_error(R"({"processing": {"workers": -1}})", "expected unsigned");
    expect_config_error(R"({"logging": {"level": "loud"}})", "logging.level");
    expect_config_error(R"({"detection": {"thresholds": {"pressure": {"warn_low": 200}}}})",
                        "detection.thresholds.pressure");
    expect_config_error("not json", "not valid JSON");
}

TEST(ServerConfig, ShippedConfigFilesLoad) {
    EXPECT_NO_THROW(load_server_config(config_path("server.json")));
    EXPECT_NO_THROW(load_server_config(config_path("scenarios/overload_server.json")));
    EXPECT_THROW(load_server_config(config_path("does_not_exist.json")), ConfigError);
}

TEST(SimulatorConfig, ExpandsGroupsIntoSensors) {
    const auto config = sim::parse_simulator_config(R"({
        "server": {"host": "example", "port": 9100},
        "groups": [
            {"id_prefix": "t", "type": "temperature", "count": 3, "nominal": 20},
            {"id_prefix": "m", "type": "motor_speed", "nominal": 3000, "rate_hz": 50,
             "faults": {"drop_probability": 0.1, "fail_after_s": 5, "fail_mode": "disconnect"}}
        ]
    })");
    EXPECT_EQ(config.host, "example");
    EXPECT_EQ(config.port, 9100);
    ASSERT_EQ(config.sensors.size(), 4u);
    EXPECT_EQ(config.sensors[0].id, "t-001");
    EXPECT_EQ(config.sensors[2].id, "t-003");
    EXPECT_EQ(config.sensors[3].id, "m-001");
    EXPECT_EQ(config.sensors[3].type, SensorType::MotorSpeed);
    EXPECT_EQ(config.sensors[3].faults.fail_mode, sim::FailMode::Disconnect);
    ASSERT_TRUE(config.sensors[3].faults.fail_after_s.has_value());
    EXPECT_DOUBLE_EQ(*config.sensors[3].faults.fail_after_s, 5.0);
}

TEST(SimulatorConfig, RejectsInvalidInput) {
    EXPECT_THROW(sim::parse_simulator_config("{}"), ConfigError);
    EXPECT_THROW(sim::parse_simulator_config(
                     R"({"groups": [{"id_prefix": "t", "type": "laser", "nominal": 1}]})"),
                 ConfigError);
    EXPECT_THROW(sim::parse_simulator_config(
                     R"({"groups": [{"id_prefix": "t", "type": "temperature"}]})"),
                 ConfigError)
        << "nominal is required";
    EXPECT_THROW(sim::parse_simulator_config(
                     R"({"groups": [{"id_prefix": "t", "type": "temperature", "nominal": 1,
                                     "faults": {"drop_probability": 1.5}}]})"),
                 ConfigError);
    EXPECT_THROW(sim::parse_simulator_config(
                     R"({"groups": [{"id_prefix": "bad id", "type": "temperature", "nominal": 1}]})"),
                 ConfigError);
}

TEST(SimulatorConfig, ShippedConfigFilesLoad) {
    for (const char* file : {"simulator.json", "scenarios/faults.json",
                             "scenarios/many_sensors.json"}) {
        EXPECT_NO_THROW(sim::load_simulator_config(config_path(file))) << file;
    }
}
