#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "telemetry/app/config_error.hpp"
#include "telemetry/common/logger.hpp"
#include "telemetry/sim/sensor_model.hpp"

namespace telemetry::sim {

struct SimulatorConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::uint64_t seed{42};
    double duration_s{0.0};  ///< 0 = run until stopped.
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds reconnect_initial{200};
    std::chrono::milliseconds reconnect_max{5000};
    std::chrono::milliseconds stats_interval{5000};
    std::vector<SensorSpec> sensors;
};

/// Loads a simulator configuration (see config/simulator*.json). Sensor groups are
/// expanded into individual sensors named "<id_prefix>-NNN". Throws ConfigError.
SimulatorConfig load_simulator_config(const std::string& path);
SimulatorConfig parse_simulator_config(const std::string& json_text);

struct SimulatorStats {
    std::uint64_t generated{0};
    std::uint64_t sent{0};
    std::uint64_t dropped_by_fault{0};
    std::uint64_t malformed_sent{0};
    std::uint64_t delayed{0};
    std::uint64_t spikes{0};
    std::uint64_t lost_while_disconnected{0};
    std::uint64_t connects{0};
    std::uint64_t connect_failures{0};
    std::uint64_t disconnects{0};
};

/// Runs one thread per simulated sensor, each with its own TCP connection.
///
/// Each sensor thread ticks at its configured rate using absolute deadlines (so timing
/// errors do not accumulate), applies the fault model and sends one frame per tick. If
/// the connection drops, the sensor keeps generating (and counting lost samples) while it
/// reconnects with exponential backoff. stop() wakes all sensor threads immediately via
/// a condition variable and joins them.
class Simulator {
public:
    Simulator(SimulatorConfig config, Logger& logger);
    ~Simulator();

    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void start();
    void stop();

    /// Blocks until stop() is called or the configured duration elapses.
    void wait();

    SimulatorStats stats() const;
    std::size_t sensor_count() const { return config_.sensors.size(); }

private:
    struct Counters {
        std::atomic<std::uint64_t> generated{0};
        std::atomic<std::uint64_t> sent{0};
        std::atomic<std::uint64_t> dropped_by_fault{0};
        std::atomic<std::uint64_t> malformed_sent{0};
        std::atomic<std::uint64_t> delayed{0};
        std::atomic<std::uint64_t> spikes{0};
        std::atomic<std::uint64_t> lost_while_disconnected{0};
        std::atomic<std::uint64_t> connects{0};
        std::atomic<std::uint64_t> connect_failures{0};
        std::atomic<std::uint64_t> disconnects{0};
    };

    void run_sensor(const SensorSpec& spec, std::uint64_t seed);
    /// Sleeps until `deadline` or stop. Returns false if stopping.
    bool sleep_until(std::chrono::steady_clock::time_point deadline);

    SimulatorConfig config_;
    Logger& logger_;
    Counters counters_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable stop_cv_;
    bool stopping_{false};
};

}  // namespace telemetry::sim
