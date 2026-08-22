#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "telemetry/app/telemetry_service.hpp"

namespace telemetry {

/// Rates and interval latencies derived from two consecutive samples.
struct IntervalStats {
    double seconds{0.0};
    double frames_per_s{0.0};
    double processed_per_s{0.0};
    double dropped_per_s{0.0};
    HistogramSnapshot processing_latency;  ///< Only samples recorded during the interval.
};

IntervalStats compute_interval(const StatusSample& previous, const StatusSample& current);

/// Formats the periodic terminal status block. Pure function of its inputs (testable).
std::string format_status(const StatusSample& current, const IntervalStats& interval,
                          double uptime_s, std::size_t max_listed_sensors);

/// One-line-per-field summary printed at shutdown.
std::string format_final_summary(const StatusSample& sample, double uptime_s);

/// Machine-readable metrics written by --metrics-json and consumed by the benchmark
/// scripts.
std::string metrics_to_json(const StatusSample& sample, double uptime_s);

}  // namespace telemetry
