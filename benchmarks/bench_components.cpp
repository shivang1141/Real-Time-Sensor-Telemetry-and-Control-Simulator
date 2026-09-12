// Micro-benchmarks for the per-message building blocks. They answer "what does each
// stage cost per message?" so pipeline results can be attributed to a stage.
//
// Build in Release (cmake --preset release) before trusting the numbers.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "telemetry/common/clock.hpp"
#include "telemetry/metrics/latency_histogram.hpp"
#include "telemetry/processing/anomaly_detector.hpp"
#include "telemetry/processing/bounded_queue.hpp"
#include "telemetry/processing/sensor_registry.hpp"
#include "telemetry/protocol/line_framer.hpp"
#include "telemetry/protocol/protocol.hpp"

using namespace telemetry;
using BenchClock = std::chrono::steady_clock;

namespace {

// Prevents the optimizer from discarding benchmarked work.
std::atomic<std::uint64_t> g_sink{0};

template <typename F>
double ns_per_op(std::uint64_t iterations, F&& body) {
    // Warm-up.
    for (std::uint64_t i = 0; i < iterations / 10 + 1; ++i) body(i);
    const auto start = BenchClock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) body(i);
    const auto elapsed = std::chrono::duration<double, std::nano>(BenchClock::now() - start).count();
    return elapsed / static_cast<double>(iterations);
}

void report(const char* name, double ns) {
    std::printf("%-44s %10.1f ns/op  %12.0f ops/s\n", name, ns, 1e9 / ns);
}

TelemetryMessage sample_message(std::uint64_t seq) {
    TelemetryMessage m;
    m.sensor_id = "temp-" + std::to_string(seq % 100);
    m.type = SensorType::Temperature;
    m.value = 25.0 + static_cast<double>(seq % 7) * 0.1;
    m.timestamp_us = SystemClock{}.wall_now_us();
    m.sequence = seq;
    return m;
}

void bench_protocol() {
    const auto message = sample_message(42);
    report("protocol::encode", ns_per_op(200'000, [&](std::uint64_t) {
               g_sink += protocol::encode(message).size();
           }));
    const std::string frame = protocol::encode(message);
    report("protocol::decode (valid frame)", ns_per_op(200'000, [&](std::uint64_t) {
               g_sink += protocol::decode(frame).message->sequence;
           }));
    const std::string garbage = "garbage %%% {{{";
    report("protocol::decode (malformed frame)", ns_per_op(200'000, [&](std::uint64_t) {
               g_sink += static_cast<std::uint64_t>(protocol::decode(garbage).error);
           }));
}

void bench_framer() {
    std::string stream;
    for (int i = 0; i < 1000; ++i) {
        stream += protocol::encode(sample_message(static_cast<std::uint64_t>(i))) + "\n";
    }
    protocol::LineFramer framer(1024);
    const double ns = ns_per_op(2'000, [&](std::uint64_t) {
        // Feed in 16 KiB chunks, as the server's recv buffer does.
        for (std::size_t pos = 0; pos < stream.size(); pos += 16384) {
            const std::size_t n = std::min<std::size_t>(16384, stream.size() - pos);
            framer.feed(
                stream.data() + pos, n, [](std::string_view f) { g_sink += f.size(); }, [] {});
        }
    });
    report("LineFramer (per frame, 16 KiB chunks)", ns / 1000.0);
}

void bench_histogram() {
    LatencyHistogram h;
    report("LatencyHistogram::record (1 thread)",
           ns_per_op(5'000'000, [&](std::uint64_t i) { h.record(i % 5000); }));

    constexpr int kThreads = 4;
    constexpr std::uint64_t kPerThread = 2'000'000;
    const auto start = BenchClock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&h] {
            for (std::uint64_t i = 0; i < kPerThread; ++i) h.record(i % 5000);
        });
    }
    for (auto& th : threads) th.join();
    const double ns = std::chrono::duration<double, std::nano>(BenchClock::now() - start).count() /
                      static_cast<double>(kThreads * kPerThread);
    report("LatencyHistogram::record (4 threads, wall)", ns);
}

void bench_queue(int producers, int consumers) {
    constexpr std::uint64_t kItems = 2'000'000;
    BoundedQueue<std::uint64_t> q(4096);
    const std::uint64_t per_producer = kItems / static_cast<std::uint64_t>(producers);
    const auto start = BenchClock::now();
    std::vector<std::thread> threads;
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            while (auto v = q.pop()) g_sink += *v;
        });
    }
    std::vector<std::thread> prod;
    for (int p = 0; p < producers; ++p) {
        prod.emplace_back([&] {
            for (std::uint64_t i = 0; i < per_producer; ++i) q.push(std::uint64_t{i});
        });
    }
    for (auto& t : prod) t.join();
    q.close();
    for (auto& t : threads) t.join();
    const double ns = std::chrono::duration<double, std::nano>(BenchClock::now() - start).count() /
                      static_cast<double>(per_producer * static_cast<std::uint64_t>(producers));
    char name[64];
    std::snprintf(name, sizeof name, "BoundedQueue push+pop (%dP/%dC, wall)", producers,
                  consumers);
    report(name, ns);
}

void bench_registry() {
    AnomalyDetector detector(DetectorConfig::defaults());
    SensorRegistry registry(detector);
    std::vector<Event> events;
    SystemClock clock;
    std::vector<TelemetryMessage> messages;
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        messages.push_back(sample_message(i));
        messages.back().sequence = i / 100;  // per-sensor contiguous sequence
    }
    std::uint64_t idx = 0;
    const auto steady = clock.steady_now();
    const auto wall = clock.wall_now_us();
    const double ns = [&] {
        const auto start = BenchClock::now();
        for (; idx < messages.size(); ++idx) {
            registry.update(messages[idx], steady, wall, events);
            events.clear();
        }
        return std::chrono::duration<double, std::nano>(BenchClock::now() - start).count() /
               static_cast<double>(messages.size());
    }();
    report("SensorRegistry::update (100 sensors)", ns);
}

}  // namespace

int main() {
    std::printf("hardware threads: %u\n\n", std::thread::hardware_concurrency());
    bench_protocol();
    bench_framer();
    bench_registry();
    bench_histogram();
    bench_queue(1, 1);
    bench_queue(4, 1);
    bench_queue(4, 4);
    std::printf("\n(sink %llu)\n", static_cast<unsigned long long>(g_sink.load() % 10));
    return 0;
}
