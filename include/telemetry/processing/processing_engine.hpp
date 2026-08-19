#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "telemetry/common/clock.hpp"
#include "telemetry/common/telemetry_message.hpp"
#include "telemetry/metrics/metrics.hpp"
#include "telemetry/processing/anomaly_detector.hpp"
#include "telemetry/processing/bounded_queue.hpp"
#include "telemetry/processing/events.hpp"
#include "telemetry/processing/sensor_registry.hpp"

namespace telemetry {

struct ProcessingConfig {
    std::size_t workers{2};
    std::size_t queue_capacity_per_worker{4096};
    std::chrono::milliseconds stale_check_interval{250};
    /// Artificial CPU work per message (busy-wait). 0 in normal operation; used by tests
    /// and benchmarks to model heavier processing and to force queue saturation.
    std::chrono::microseconds simulated_work{0};
};

enum class SubmitResult { Accepted, QueueFull, ShuttingDown };

/// Producer-consumer processing stage.
///
/// Each worker owns one shard: a bounded queue plus the SensorRegistry for the sensors
/// hashed to it. Network threads call submit(), which routes by hash(sensor_id).
///
/// Why sharded queues instead of one shared queue: with a shared queue, two workers can
/// pop consecutive messages of the same sensor and finish them out of order, which
/// would corrupt sequence-gap detection and trend windows. Routing by sensor keeps each
/// sensor's messages in order on a single thread and means sensor state is never written
/// by more than one thread. The cost is possible imbalance when a few sensors dominate
/// traffic; the benchmarks use many sensors so this does not show there.
///
/// Overload policy: submit() never blocks. If the target shard's queue is full the
/// message is dropped and QueueFull is returned (the caller counts it). Blocking would
/// propagate backpressure into TCP, but it would also stall every other sensor sharing
/// that connection thread; for telemetry, newer data is more valuable than old.
///
/// Shutdown: stop() closes all queues; workers finish every message already queued, then
/// exit and are joined. submit() after stop() returns ShuttingDown.
class ProcessingEngine {
public:
    ProcessingEngine(ProcessingConfig config, DetectorConfig detector_config, Metrics& metrics,
                     EventSink& events, const Clock& clock);
    ~ProcessingEngine();

    ProcessingEngine(const ProcessingEngine&) = delete;
    ProcessingEngine& operator=(const ProcessingEngine&) = delete;

    void start();
    void stop();

    /// Thread-safe and non-blocking.
    SubmitResult submit(TelemetryMessage&& message, SteadyTime received_at);

    std::size_t queue_depth() const;
    std::size_t queue_capacity() const;
    std::size_t queue_high_water_mark() const;  ///< Max over shards of each shard's peak.
    std::size_t worker_count() const { return shards_.size(); }

    std::vector<SensorSnapshot> sensors() const;
    HealthSummary health() const;

private:
    struct WorkItem {
        TelemetryMessage message;
        SteadyTime received_at;
        SteadyTime enqueued_at;
    };

    struct Shard {
        Shard(std::size_t capacity, const AnomalyDetector& detector)
            : queue(capacity), registry(detector) {}
        BoundedQueue<WorkItem> queue;
        SensorRegistry registry;
        std::thread thread;
    };

    void worker_loop(Shard& shard);
    void process(Shard& shard, WorkItem& item, std::vector<Event>& events);
    std::size_t shard_for(const std::string& sensor_id) const;

    ProcessingConfig config_;
    AnomalyDetector detector_;
    Metrics& metrics_;
    EventSink& events_;
    const Clock& clock_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<bool> accepting_{false};
    bool started_{false};
};

}  // namespace telemetry
