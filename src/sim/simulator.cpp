#include "telemetry/sim/simulator.hpp"

#include <algorithm>
#include <system_error>

#include "telemetry/common/clock.hpp"
#include "telemetry/metrics/metrics.hpp"
#include "telemetry/net/tcp_client.hpp"
#include "telemetry/protocol/protocol.hpp"

namespace telemetry::sim {

namespace {
constexpr const char* kComponent = "sim";

std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t index) {
    // SplitMix64 finalizer: decorrelates per-sensor seeds derived from one base seed.
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (index + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
}  // namespace

Simulator::Simulator(SimulatorConfig config, Logger& logger)
    : config_(std::move(config)), logger_(logger) {}

Simulator::~Simulator() { stop(); }

void Simulator::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    threads_.reserve(config_.sensors.size());
    for (std::size_t i = 0; i < config_.sensors.size(); ++i) {
        const SensorSpec& spec = config_.sensors[i];
        const std::uint64_t seed = mix_seed(config_.seed, i);
        threads_.emplace_back([this, &spec, seed] { run_sensor(spec, seed); });
    }
    logger_.info(kComponent, "started",
                 {{"sensors", config_.sensors.size()},
                  {"server", config_.host + ":" + std::to_string(config_.port)}});
}

void Simulator::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    stop_cv_.notify_all();
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void Simulator::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (config_.duration_s > 0) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(config_.duration_s));
        stop_cv_.wait_until(lock, deadline, [this] { return stopping_; });
    } else {
        stop_cv_.wait(lock, [this] { return stopping_; });
    }
}

bool Simulator::sleep_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !stop_cv_.wait_until(lock, deadline, [this] { return stopping_; });
}

void Simulator::run_sensor(const SensorSpec& spec, std::uint64_t seed) {
    using Clock = std::chrono::steady_clock;
    SensorModel model(spec, seed);
    net::TcpClient client;
    const SystemClock wall;

    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / spec.rate_hz));
    const Clock::time_point start = Clock::now();
    Clock::time_point next_tick = start;
    Clock::time_point next_connect_attempt = start;
    auto backoff = config_.reconnect_initial;

    auto try_connect = [&](Clock::time_point now) {
        if (now < next_connect_attempt) {
            return false;
        }
        try {
            client.connect(config_.host, config_.port, config_.connect_timeout);
            increment(counters_.connects);
            backoff = config_.reconnect_initial;
            logger_.debug(kComponent, "sensor_connected", {{"sensor", spec.id}});
            return true;
        } catch (const std::system_error& e) {
            increment(counters_.connect_failures);
            next_connect_attempt = now + backoff;
            logger_.debug(kComponent, "connect_failed",
                          {{"sensor", spec.id}, {"error", e.what()},
                           {"retry_ms", static_cast<std::int64_t>(backoff.count())}});
            backoff = std::min(backoff * 2, config_.reconnect_max);
            return false;
        }
    };

    while (sleep_until(next_tick)) {
        next_tick += period;
        const Clock::time_point now = Clock::now();
        if (next_tick < now) {
            next_tick = now;  // Fell behind (e.g. after a delay fault): do not burst.
        }
        const double elapsed_s = std::chrono::duration<double>(now - start).count();

        if (model.failed(elapsed_s)) {
            if (spec.faults.fail_mode == FailMode::Silent) {
                continue;
            }
            if (spec.faults.fail_mode == FailMode::Disconnect) {
                if (client.connected()) {
                    client.close();
                    increment(counters_.disconnects);
                    logger_.info(kComponent, "sensor_failed_disconnect", {{"sensor", spec.id}});
                }
                next_connect_attempt = now;
                continue;
            }
            // FaultStatus: keep sending; the model marks status=fault.
        }

        SensorModel::Step step = model.next(elapsed_s, wall.wall_now_us());
        increment(counters_.generated);
        if (step.spiked) {
            increment(counters_.spikes);
        }
        if (step.action == SensorModel::Action::Drop) {
            increment(counters_.dropped_by_fault);
            continue;
        }
        if (step.delay.count() > 0) {
            increment(counters_.delayed);
            if (!sleep_until(Clock::now() + step.delay)) {
                break;
            }
        }

        if (!client.connected() && !try_connect(Clock::now())) {
            increment(counters_.lost_while_disconnected);
            continue;
        }

        std::string frame = protocol::encode(step.message);
        if (step.action == SensorModel::Action::SendMalformed) {
            frame = model.corrupt(frame);
        }
        if (client.send_line(frame)) {
            increment(counters_.sent);
            if (step.action == SensorModel::Action::SendMalformed) {
                increment(counters_.malformed_sent);
            }
        } else {
            increment(counters_.disconnects);
            increment(counters_.lost_while_disconnected);
            next_connect_attempt = Clock::now() + backoff;
            logger_.warn(kComponent, "send_failed", {{"sensor", spec.id}});
        }
    }
    client.close();
}

SimulatorStats Simulator::stats() const {
    auto load = [](const std::atomic<std::uint64_t>& c) { return c.load(std::memory_order_relaxed); };
    SimulatorStats s;
    s.generated = load(counters_.generated);
    s.sent = load(counters_.sent);
    s.dropped_by_fault = load(counters_.dropped_by_fault);
    s.malformed_sent = load(counters_.malformed_sent);
    s.delayed = load(counters_.delayed);
    s.spikes = load(counters_.spikes);
    s.lost_while_disconnected = load(counters_.lost_while_disconnected);
    s.connects = load(counters_.connects);
    s.connect_failures = load(counters_.connect_failures);
    s.disconnects = load(counters_.disconnects);
    return s;
}

}  // namespace telemetry::sim
