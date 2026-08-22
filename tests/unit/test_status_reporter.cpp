#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "telemetry/app/status_reporter.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

namespace {

StatusSample make_sample(SteadyTime at, std::uint64_t frames, std::uint64_t processed,
                         std::uint64_t dropped) {
    StatusSample s;
    s.at = at;
    s.metrics.frames_received = frames;
    s.metrics.messages_processed = processed;
    s.metrics.messages_dropped = dropped;
    s.metrics.processing_latency.buckets.resize(LatencyHistogram::kBucketCount);
    return s;
}

}  // namespace

TEST(StatusReporter, IntervalRatesUseElapsedTime) {
    const SteadyTime t0{};
    const auto a = make_sample(t0, 1000, 900, 0);
    const auto b = make_sample(t0 + 2s, 5000, 4900, 100);
    const auto interval = compute_interval(a, b);
    EXPECT_DOUBLE_EQ(interval.seconds, 2.0);
    EXPECT_DOUBLE_EQ(interval.frames_per_s, 2000.0);
    EXPECT_DOUBLE_EQ(interval.processed_per_s, 2000.0);
    EXPECT_DOUBLE_EQ(interval.dropped_per_s, 50.0);
}

TEST(StatusReporter, ZeroElapsedTimeGivesZeroRates) {
    const auto a = make_sample(SteadyTime{}, 10, 10, 0);
    const auto interval = compute_interval(a, a);
    EXPECT_EQ(interval.frames_per_s, 0.0);
}

TEST(StatusReporter, StatusListsOnlyUnhealthySensors) {
    auto s = make_sample(SteadyTime{} + 1s, 10, 10, 0);
    SensorSnapshot ok;
    ok.id = "healthy-1";
    SensorSnapshot hot;
    hot.id = "hot-1";
    hot.health = SensorHealth::Critical;
    hot.active = bit(Condition::WarnHigh) | bit(Condition::CritHigh);
    s.sensors = {ok, hot};
    s.health.add(ok);
    s.health.add(hot);

    const std::string text = format_status(s, IntervalStats{}, 1.0, 10);
    EXPECT_NE(text.find("hot-1"), std::string::npos);
    EXPECT_NE(text.find("warn_high,crit_high"), std::string::npos);
    EXPECT_EQ(text.find("healthy-1"), std::string::npos);
    EXPECT_NE(text.find("total=2 ok=1"), std::string::npos);
}

TEST(StatusReporter, MetricsJsonIsParseableAndComplete) {
    auto s = make_sample(SteadyTime{}, 10, 9, 1);
    s.metrics.queue_wait.buckets.resize(LatencyHistogram::kBucketCount);
    s.metrics.end_to_end_latency.buckets.resize(LatencyHistogram::kBucketCount);
    const auto json = nlohmann::json::parse(metrics_to_json(s, 12.5));
    EXPECT_EQ(json["messages"]["processed"], 9);
    EXPECT_EQ(json["messages"]["dropped"], 1);
    EXPECT_DOUBLE_EQ(json["uptime_s"].get<double>(), 12.5);
    EXPECT_TRUE(json["processing_latency"].contains("p95_us"));
}
