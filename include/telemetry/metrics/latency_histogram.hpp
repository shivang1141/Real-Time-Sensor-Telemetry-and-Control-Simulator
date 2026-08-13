#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry {

/// Point-in-time copy of a LatencyHistogram. Snapshots can be subtracted to get the
/// distribution for an interval (e.g. the last second) rather than since start-up.
struct HistogramSnapshot {
    std::vector<std::uint64_t> buckets;
    std::uint64_t count{0};
    std::uint64_t sum_us{0};
    std::uint64_t max_us{0};  ///< Exact maximum since start (not meaningful after subtraction).

    /// Value at quantile q in [0, 1], reported as the upper bound of the bucket holding
    /// the ceil(q * count)-th smallest sample (so it never under-reports). Returns 0 if
    /// empty.
    std::uint64_t percentile(double q) const;
    double mean() const { return count == 0 ? 0.0 : static_cast<double>(sum_us) / static_cast<double>(count); }

    /// Distribution of samples recorded after `earlier` was taken.
    HistogramSnapshot since(const HistogramSnapshot& earlier) const;
};

/// Fixed-size log-linear latency histogram in microseconds.
///
/// Values below 64 us get exact buckets. Above that, each power-of-two range is split
/// into 32 sub-buckets, so the relative error of a reported percentile is below
/// 1/32 (~3.1%). Values of 2^38 us (~76 h) or more land in the last bucket.
///
/// record() is wait-free (relaxed atomic increments), so many worker threads can record
/// concurrently without a lock. snapshot() reads the counters individually, so a
/// snapshot taken during recording may be off by the few samples in flight; that is
/// acceptable for monitoring.
class LatencyHistogram {
public:
    static constexpr std::size_t kLinearBuckets = 64;
    static constexpr std::size_t kSubBucketBits = 5;
    static constexpr std::size_t kSubBuckets = 1u << kSubBucketBits;
    static constexpr std::size_t kMaxExponent = 37;
    static constexpr std::size_t kBucketCount =
        kLinearBuckets + (kMaxExponent - 6 + 1) * kSubBuckets;

    void record(std::uint64_t micros);
    void record(std::chrono::nanoseconds duration);
    HistogramSnapshot snapshot() const;

    static std::size_t bucket_index(std::uint64_t micros);
    static std::uint64_t bucket_upper_bound(std::size_t index);

private:
    std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_us_{0};
    std::atomic<std::uint64_t> max_us_{0};
};

}  // namespace telemetry
