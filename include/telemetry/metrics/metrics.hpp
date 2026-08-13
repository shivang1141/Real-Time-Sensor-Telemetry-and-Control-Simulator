#pragma once

#include <atomic>
#include <cstdint>

#include "telemetry/metrics/latency_histogram.hpp"

namespace telemetry {

/// Copy of every counter at one instant. See docs/METRICS.md for definitions.
struct MetricsSnapshot {
    std::uint64_t connections_accepted{0};
    std::uint64_t connections_rejected{0};
    std::uint64_t connections_closed{0};
    std::uint64_t frames_received{0};
    std::uint64_t frames_oversized{0};
    std::uint64_t messages_invalid{0};
    std::uint64_t messages_enqueued{0};
    std::uint64_t messages_dropped{0};
    std::uint64_t messages_processed{0};
    std::uint64_t messages_rejected{0};
    std::uint64_t sequence_missing{0};
    HistogramSnapshot processing_latency;
    HistogramSnapshot queue_wait;
    HistogramSnapshot end_to_end_latency;
};

/// Process-wide counters shared by network and processing threads.
///
/// All counters are monotonic and updated with relaxed atomics: they are statistics,
/// not synchronization, so no ordering between them is required. A snapshot is
/// therefore not an atomic cut across counters; invariants such as
///   frames_received == messages_invalid + messages_dropped + messages_enqueued
/// hold exactly once traffic has stopped and may be off by in-flight messages while
/// it is running.
struct Metrics {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> connections_closed{0};

    std::atomic<std::uint64_t> frames_received{0};   ///< Complete frames read off sockets.
    std::atomic<std::uint64_t> frames_oversized{0};  ///< Frames discarded by the size limit.
    std::atomic<std::uint64_t> messages_invalid{0};  ///< Frames that failed to decode.
    std::atomic<std::uint64_t> messages_enqueued{0};
    std::atomic<std::uint64_t> messages_dropped{0};  ///< Queue full at submit time.

    std::atomic<std::uint64_t> messages_processed{0};  ///< Dequeued and handled.
    std::atomic<std::uint64_t> messages_rejected{0};   ///< Processed but semantically invalid.
    std::atomic<std::uint64_t> sequence_missing{0};    ///< Sum of detected sequence gaps.

    LatencyHistogram processing_latency;  ///< Frame received -> processing finished (steady).
    LatencyHistogram queue_wait;          ///< Enqueue -> dequeue (steady).
    LatencyHistogram end_to_end_latency;  ///< Sender timestamp -> processed (wall clocks).

    MetricsSnapshot snapshot() const;
};

inline void increment(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) {
    counter.fetch_add(by, std::memory_order_relaxed);
}

}  // namespace telemetry
