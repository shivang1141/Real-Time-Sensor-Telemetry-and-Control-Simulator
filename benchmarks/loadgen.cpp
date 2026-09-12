// telemetry_loadgen: high-rate load generator for benchmarking the server.
//
// Unlike sensor_sim (one thread per sensor, realistic timing, fault injection), the load
// generator multiplexes many sensors over a few connections and batches frames into
// large writes, so that the generator itself is not the bottleneck. It uses a
// hand-written encoder producing the same wire format as protocol::encode, which is
// cheaper than building a JSON object per message.
//
// Prints a JSON summary to stdout.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "common/cli.hpp"
#include "telemetry/common/clock.hpp"
#include "telemetry/net/tcp_client.hpp"

namespace {

constexpr const char* kUsage = R"(Usage: telemetry_loadgen [options]
  --host <host>                    default 127.0.0.1
  --port <n>                       default 9000
  --connections <n>                default 8
  --sensors-per-connection <n>     default 10
  --rate <msg/s>                   total target rate across all connections; 0 = max (default)
  --duration <s>                   default 10
  --batch <n>                      frames per write (default 32)
)";

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    unsigned connections = 8;
    unsigned sensors_per_connection = 10;
    double rate = 0;
    double duration_s = 10;
    unsigned batch = 32;
};

void append_frame(std::string& out, const std::string& id, std::uint64_t seq, double value,
                  std::int64_t ts) {
    char buffer[192];
    const int n = std::snprintf(
        buffer, sizeof buffer,
        R"({"id":"%s","seq":%llu,"status":"ok","ts":%lld,"type":"temperature","value":%.3f})"
        "\n",
        id.c_str(), static_cast<unsigned long long>(seq), static_cast<long long>(ts), value);
    out.append(buffer, static_cast<std::size_t>(n));
}

}  // namespace

int main(int argc, char** argv) {
    using namespace telemetry;
    cli::Args args(argc, argv);
    if (args.flag("--help")) {
        std::cout << kUsage;
        return 0;
    }
    Options opt;
    if (auto v = args.value("--host")) opt.host = *v;
    if (auto v = args.number<std::uint16_t>("--port")) opt.port = *v;
    if (auto v = args.number<unsigned>("--connections")) opt.connections = *v;
    if (auto v = args.number<unsigned>("--sensors-per-connection")) opt.sensors_per_connection = *v;
    if (auto v = args.number<double>("--rate")) opt.rate = *v;
    if (auto v = args.number<double>("--duration")) opt.duration_s = *v;
    if (auto v = args.number<unsigned>("--batch")) opt.batch = *v;
    args.reject_unknown();
    if (opt.connections == 0 || opt.sensors_per_connection == 0 || opt.batch == 0) {
        cli::Args::fail("connections, sensors-per-connection and batch must be > 0");
    }

    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<bool> stop{false};
    const double per_connection_rate = opt.rate / opt.connections;

    std::vector<std::thread> threads;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned c = 0; c < opt.connections; ++c) {
        threads.emplace_back([&, c] {
            net::TcpClient client;
            try {
                client.connect(opt.host, opt.port, std::chrono::seconds(2),
                               std::chrono::seconds(5));
            } catch (const std::exception& e) {
                std::cerr << "connect failed: " << e.what() << "\n";
                errors.fetch_add(1);
                return;
            }
            std::vector<std::string> ids;
            for (unsigned s = 0; s < opt.sensors_per_connection; ++s) {
                ids.push_back("lg-" + std::to_string(c) + "-" + std::to_string(s));
            }
            std::vector<std::uint64_t> seq(ids.size(), 0);
            std::string batch;
            batch.reserve(opt.batch * 128);
            std::uint64_t local_sent = 0;
            std::size_t next_sensor = 0;
            const SystemClock wall;
            const auto thread_start = std::chrono::steady_clock::now();

            while (!stop.load(std::memory_order_relaxed)) {
                if (per_connection_rate > 0) {
                    const double elapsed = std::chrono::duration<double>(
                                               std::chrono::steady_clock::now() - thread_start)
                                               .count();
                    if (static_cast<double>(local_sent) >= per_connection_rate * elapsed) {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        continue;
                    }
                }
                batch.clear();
                const std::int64_t ts = wall.wall_now_us();
                for (unsigned i = 0; i < opt.batch; ++i) {
                    const std::size_t s = next_sensor;
                    next_sensor = (next_sensor + 1) % ids.size();
                    // Each sensor reports a constant in-range value: benchmark traffic is
                    // deliberately "healthy" so it measures the pipeline, not alarm
                    // handling. (Varying values at microsecond spacing would correctly
                    // trigger rate-of-change alarms and flood the log.)
                    append_frame(batch, ids[s], seq[s]++, 20.0 + static_cast<double>(s % 10), ts);
                }
                if (!client.send_raw(batch)) {
                    errors.fetch_add(1);
                    return;
                }
                local_sent += opt.batch;
                sent.fetch_add(opt.batch, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(opt.duration_s));
    stop.store(true);
    for (auto& t : threads) t.join();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf(
        "{\"connections\":%u,\"sensors\":%u,\"target_rate\":%.0f,\"duration_s\":%.3f,"
        "\"sent\":%llu,\"send_rate\":%.1f,\"errors\":%llu}\n",
        opt.connections, opt.connections * opt.sensors_per_connection, opt.rate, elapsed,
        static_cast<unsigned long long>(sent.load()), static_cast<double>(sent.load()) / elapsed,
        static_cast<unsigned long long>(errors.load()));
    return errors.load() == 0 ? 0 : 1;
}
