#include "telemetry/processing/processing_engine.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace telemetry {

ProcessingEngine::ProcessingEngine(ProcessingConfig config, DetectorConfig detector_config,
                                   Metrics& metrics, EventSink& events, const Clock& clock)
    : config_(config),
      detector_(std::move(detector_config)),
      metrics_(metrics),
      events_(events),
      clock_(clock) {
    if (config_.workers == 0) {
        throw std::invalid_argument("ProcessingEngine requires at least one worker");
    }
    shards_.reserve(config_.workers);
    for (std::size_t i = 0; i < config_.workers; ++i) {
        shards_.push_back(std::make_unique<Shard>(config_.queue_capacity_per_worker, detector_));
    }
}

ProcessingEngine::~ProcessingEngine() { stop(); }

void ProcessingEngine::start() {
    if (started_) {
        return;
    }
    started_ = true;
    accepting_.store(true);
    for (auto& shard : shards_) {
        Shard& ref = *shard;
        ref.thread = std::thread([this, &ref] { worker_loop(ref); });
    }
}

void ProcessingEngine::stop() {
    if (!accepting_.exchange(false) && !started_) {
        return;
    }
    for (auto& shard : shards_) {
        shard->queue.close();
    }
    for (auto& shard : shards_) {
        if (shard->thread.joinable()) {
            shard->thread.join();
        }
    }
    started_ = false;
}

std::size_t ProcessingEngine::shard_for(const std::string& sensor_id) const {
    return std::hash<std::string>{}(sensor_id) % shards_.size();
}

SubmitResult ProcessingEngine::submit(TelemetryMessage&& message, SteadyTime received_at) {
    if (!accepting_.load(std::memory_order_acquire)) {
        return SubmitResult::ShuttingDown;
    }
    Shard& shard = *shards_[shard_for(message.sensor_id)];
    WorkItem item{std::move(message), received_at, clock_.steady_now()};
    if (shard.queue.try_push(std::move(item))) {
        increment(metrics_.messages_enqueued);
        return SubmitResult::Accepted;
    }
    if (shard.queue.closed()) {
        return SubmitResult::ShuttingDown;
    }
    increment(metrics_.messages_dropped);
    return SubmitResult::QueueFull;
}

void ProcessingEngine::worker_loop(Shard& shard) {
    // Draining up to kMaxBatch items per lock acquisition cuts mutex traffic on the shard
    // queue; see docs/PERFORMANCE.md for the measurements that motivated it.
    constexpr std::size_t kMaxBatch = 64;
    std::vector<WorkItem> batch;
    batch.reserve(kMaxBatch);
    std::vector<Event> events;
    events.reserve(16);
    SteadyTime next_stale_check = clock_.steady_now() + config_.stale_check_interval;

    while (true) {
        batch.clear();
        const std::size_t n = shard.queue.pop_batch(batch, kMaxBatch, config_.stale_check_interval);
        for (WorkItem& item : batch) {
            process(shard, item, events);
        }
        if (n == 0 && shard.queue.closed()) {
            break;  // Closed and fully drained.
        }

        // Staleness is checked by the owning worker, so registry writes stay on one thread.
        const SteadyTime now = clock_.steady_now();
        if (now >= next_stale_check) {
            shard.registry.check_stale(now, clock_.wall_now_us(), events);
            next_stale_check = now + config_.stale_check_interval;
        }
        for (const Event& event : events) {
            events_.emit(event);
        }
        events.clear();
    }
}

void ProcessingEngine::process(Shard& shard, WorkItem& item, std::vector<Event>& events) {
    const SteadyTime started = clock_.steady_now();
    metrics_.queue_wait.record(started - item.enqueued_at);

    if (config_.simulated_work.count() > 0) {
        // Busy-wait rather than sleep: models CPU-bound work and keeps the core occupied.
        const SteadyTime until = started + config_.simulated_work;
        while (clock_.steady_now() < until) {
        }
    }

    const std::int64_t wall_now = clock_.wall_now_us();
    const auto result = shard.registry.update(item.message, started, wall_now, events);
    if (!result.accepted) {
        increment(metrics_.messages_rejected);
    }
    if (result.missing > 0) {
        increment(metrics_.sequence_missing, result.missing);
    }

    const SteadyTime finished = clock_.steady_now();
    metrics_.processing_latency.record(finished - item.received_at);
    // End-to-end latency compares the sender's wall clock with ours: only meaningful when
    // both run on one host or are NTP/PTP-synchronized. Negative values (clock skew) are
    // clamped to 0 by the histogram.
    const std::int64_t e2e_us = clock_.wall_now_us() - item.message.timestamp_us;
    metrics_.end_to_end_latency.record(std::chrono::microseconds(e2e_us));
    increment(metrics_.messages_processed);
}

std::size_t ProcessingEngine::queue_depth() const {
    std::size_t depth = 0;
    for (const auto& shard : shards_) {
        depth += shard->queue.size();
    }
    return depth;
}

std::size_t ProcessingEngine::queue_capacity() const {
    return config_.queue_capacity_per_worker * shards_.size();
}

std::size_t ProcessingEngine::queue_high_water_mark() const {
    std::size_t peak = 0;
    for (const auto& shard : shards_) {
        peak = std::max(peak, shard->queue.high_water_mark());
    }
    return peak;
}

std::vector<SensorSnapshot> ProcessingEngine::sensors() const {
    const SteadyTime now = clock_.steady_now();
    std::vector<SensorSnapshot> all;
    for (const auto& shard : shards_) {
        auto part = shard->registry.snapshot(now);
        all.insert(all.end(), std::make_move_iterator(part.begin()),
                   std::make_move_iterator(part.end()));
    }
    std::sort(all.begin(), all.end(),
              [](const SensorSnapshot& a, const SensorSnapshot& b) { return a.id < b.id; });
    return all;
}

HealthSummary ProcessingEngine::health() const {
    HealthSummary total;
    for (const auto& shard : shards_) {
        total.merge(shard->registry.summary());
    }
    return total;
}

}  // namespace telemetry
