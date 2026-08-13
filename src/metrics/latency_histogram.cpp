#include "telemetry/metrics/latency_histogram.hpp"

#include <algorithm>
#include <cmath>

namespace telemetry {

namespace {

std::size_t floor_log2(std::uint64_t value) {
    return static_cast<std::size_t>(63 - __builtin_clzll(value));
}

}  // namespace

std::size_t LatencyHistogram::bucket_index(std::uint64_t micros) {
    if (micros < kLinearBuckets) {
        return static_cast<std::size_t>(micros);
    }
    const std::size_t exponent = floor_log2(micros);  // >= 6
    if (exponent > kMaxExponent) {
        return kBucketCount - 1;
    }
    const std::size_t sub =
        static_cast<std::size_t>(micros >> (exponent - kSubBucketBits)) & (kSubBuckets - 1);
    return kLinearBuckets + (exponent - 6) * kSubBuckets + sub;
}

std::uint64_t LatencyHistogram::bucket_upper_bound(std::size_t index) {
    if (index < kLinearBuckets) {
        return index;
    }
    const std::size_t offset = index - kLinearBuckets;
    const std::size_t exponent = 6 + offset / kSubBuckets;
    const std::uint64_t sub = offset % kSubBuckets;
    const std::uint64_t width = std::uint64_t{1} << (exponent - kSubBucketBits);
    const std::uint64_t lower = (kSubBuckets + sub) * width;
    return lower + width - 1;
}

void LatencyHistogram::record(std::uint64_t micros) {
    buckets_[bucket_index(micros)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_us_.fetch_add(micros, std::memory_order_relaxed);
    std::uint64_t current = max_us_.load(std::memory_order_relaxed);
    while (micros > current &&
           !max_us_.compare_exchange_weak(current, micros, std::memory_order_relaxed)) {
    }
}

void LatencyHistogram::record(std::chrono::nanoseconds duration) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    record(micros > 0 ? static_cast<std::uint64_t>(micros) : 0);
}

HistogramSnapshot LatencyHistogram::snapshot() const {
    HistogramSnapshot snap;
    snap.buckets.resize(kBucketCount);
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        snap.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
    }
    // Derive count from the buckets so percentile ranks are consistent with them.
    for (const std::uint64_t b : snap.buckets) {
        snap.count += b;
    }
    snap.sum_us = sum_us_.load(std::memory_order_relaxed);
    snap.max_us = max_us_.load(std::memory_order_relaxed);
    return snap;
}

std::uint64_t HistogramSnapshot::percentile(double q) const {
    if (count == 0 || buckets.empty()) {
        return 0;
    }
    q = std::clamp(q, 0.0, 1.0);
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count))));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        seen += buckets[i];
        if (seen >= rank) {
            return LatencyHistogram::bucket_upper_bound(i);
        }
    }
    return LatencyHistogram::bucket_upper_bound(buckets.size() - 1);
}

HistogramSnapshot HistogramSnapshot::since(const HistogramSnapshot& earlier) const {
    HistogramSnapshot delta;
    delta.buckets.resize(buckets.size());
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        const std::uint64_t before = i < earlier.buckets.size() ? earlier.buckets[i] : 0;
        delta.buckets[i] = buckets[i] >= before ? buckets[i] - before : 0;
        delta.count += delta.buckets[i];
    }
    delta.sum_us = sum_us >= earlier.sum_us ? sum_us - earlier.sum_us : 0;
    delta.max_us = 0;
    for (std::size_t i = delta.buckets.size(); i-- > 0;) {
        if (delta.buckets[i] != 0) {
            delta.max_us = LatencyHistogram::bucket_upper_bound(i);
            break;
        }
    }
    return delta;
}

}  // namespace telemetry
