#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include "telemetry/metrics/latency_histogram.hpp"

using telemetry::HistogramSnapshot;
using telemetry::LatencyHistogram;

TEST(LatencyHistogram, EmptySnapshot) {
    LatencyHistogram h;
    const auto s = h.snapshot();
    EXPECT_EQ(s.count, 0u);
    EXPECT_EQ(s.percentile(0.5), 0u);
    EXPECT_EQ(s.mean(), 0.0);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 10; ++v) {
        h.record(v);
    }
    const auto s = h.snapshot();
    EXPECT_EQ(s.count, 10u);
    EXPECT_EQ(s.percentile(0.5), 5u);
    EXPECT_EQ(s.percentile(0.9), 9u);
    EXPECT_EQ(s.percentile(1.0), 10u);
    EXPECT_EQ(s.max_us, 10u);
    EXPECT_DOUBLE_EQ(s.mean(), 5.5);
}

TEST(LatencyHistogram, BucketBoundsContainValue) {
    // Every value maps to a bucket whose upper bound is >= the value and within ~3.1%.
    for (std::uint64_t v : {0ull, 1ull, 63ull, 64ull, 65ull, 127ull, 128ull, 1000ull, 4095ull,
                            123456ull, 10'000'000ull, (1ull << 37) + 12345}) {
        const auto idx = LatencyHistogram::bucket_index(v);
        const auto upper = LatencyHistogram::bucket_upper_bound(idx);
        EXPECT_GE(upper, v) << v;
        EXPECT_LE(static_cast<double>(upper - v), static_cast<double>(v) / 32.0 + 1.0) << v;
        if (idx > 0) {
            EXPECT_LT(LatencyHistogram::bucket_upper_bound(idx - 1), v) << v;
        }
    }
}

TEST(LatencyHistogram, HugeValuesClampToLastBucket) {
    EXPECT_EQ(LatencyHistogram::bucket_index(~0ull), LatencyHistogram::kBucketCount - 1);
}

TEST(LatencyHistogram, PercentilesWithinRelativeErrorOfExact) {
    LatencyHistogram h;
    std::mt19937 rng(1);
    std::lognormal_distribution<double> dist(6.0, 1.0);  // median ~400 us
    std::vector<std::uint64_t> values;
    for (int i = 0; i < 100'000; ++i) {
        const auto v = static_cast<std::uint64_t>(dist(rng));
        values.push_back(v);
        h.record(v);
    }
    std::sort(values.begin(), values.end());
    const auto s = h.snapshot();
    for (double q : {0.5, 0.95, 0.99}) {
        const auto rank = static_cast<std::size_t>(std::ceil(q * values.size())) - 1;
        const double exact = static_cast<double>(values[rank]);
        const double reported = static_cast<double>(s.percentile(q));
        EXPECT_GE(reported, exact) << "q=" << q;
        EXPECT_LE(reported, exact * (1.0 + 1.0 / 32.0) + 1.0) << "q=" << q;
    }
}

TEST(LatencyHistogram, IntervalSnapshotSubtractsEarlier) {
    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) h.record(10);
    const auto first = h.snapshot();
    for (int i = 0; i < 50; ++i) h.record(1000);
    const auto delta = h.snapshot().since(first);
    EXPECT_EQ(delta.count, 50u);
    EXPECT_GE(delta.percentile(0.5), 1000u);
    EXPECT_GE(delta.max_us, 1000u);
    EXPECT_DOUBLE_EQ(delta.mean(), 1000.0);
}

TEST(LatencyHistogram, ConcurrentRecordingLosesNothing) {
    LatencyHistogram h;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50'000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&h, t] {
            for (int i = 0; i < kPerThread; ++i) {
                h.record(static_cast<std::uint64_t>(t * 100 + i % 100));
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(h.snapshot().count, static_cast<std::uint64_t>(kThreads) * kPerThread);
    EXPECT_EQ(h.snapshot().max_us, 799u);
}

TEST(LatencyHistogram, DurationOverloadConvertsAndClampsNegative) {
    LatencyHistogram h;
    h.record(std::chrono::microseconds(250));
    h.record(std::chrono::nanoseconds(-5000));
    const auto s = h.snapshot();
    EXPECT_EQ(s.count, 2u);
    EXPECT_EQ(s.percentile(0.0), 0u);
    EXPECT_EQ(s.max_us, 250u);
}
