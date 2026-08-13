#include "telemetry/metrics/metrics.hpp"

namespace telemetry {

MetricsSnapshot Metrics::snapshot() const {
    auto load = [](const std::atomic<std::uint64_t>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    MetricsSnapshot snap;
    snap.connections_accepted = load(connections_accepted);
    snap.connections_rejected = load(connections_rejected);
    snap.connections_closed = load(connections_closed);
    snap.frames_received = load(frames_received);
    snap.frames_oversized = load(frames_oversized);
    snap.messages_invalid = load(messages_invalid);
    snap.messages_enqueued = load(messages_enqueued);
    snap.messages_dropped = load(messages_dropped);
    snap.messages_processed = load(messages_processed);
    snap.messages_rejected = load(messages_rejected);
    snap.sequence_missing = load(sequence_missing);
    snap.processing_latency = processing_latency.snapshot();
    snap.queue_wait = queue_wait.snapshot();
    snap.end_to_end_latency = end_to_end_latency.snapshot();
    return snap;
}

}  // namespace telemetry
