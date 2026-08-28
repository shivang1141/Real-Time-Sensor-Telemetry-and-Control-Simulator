// sensor_sim: runs a fleet of simulated sensors that stream telemetry to the server.

#include <csignal>
#include <ctime>
#include <iostream>
#include <thread>

#include "common/cli.hpp"
#include "telemetry/sim/simulator.hpp"

namespace {

constexpr const char* kUsage = R"(Usage: sensor_sim --config <path> [options]

Options:
  --config <path>       Simulator configuration (see config/simulator*.json)
  --host <host>         Override server.host
  --port <n>            Override server.port
  --duration <seconds>  Override duration_s (0 = until Ctrl+C)
  --seed <n>            Override the random seed
  --log-level <level>   debug | info | warn | error | off (default: info)
  --help                Show this help
)";

void print_stats(const telemetry::sim::SimulatorStats& s, double elapsed_s) {
    std::cout << "[sim t=" << static_cast<long long>(elapsed_s) << "s] generated=" << s.generated
              << " sent=" << s.sent << " fault_dropped=" << s.dropped_by_fault
              << " malformed=" << s.malformed_sent << " delayed=" << s.delayed
              << " spikes=" << s.spikes << " lost_disconnected=" << s.lost_while_disconnected
              << " connects=" << s.connects << " connect_failures=" << s.connect_failures
              << " disconnects=" << s.disconnects << "\n"
              << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace telemetry;
    cli::Args args(argc, argv);
    if (args.flag("--help") || args.flag("-h")) {
        std::cout << kUsage;
        return 0;
    }

    sim::SimulatorConfig config;
    LogLevel log_level = LogLevel::Info;
    try {
        const auto path = args.value("--config");
        if (!path) {
            cli::Args::fail("--config is required");
        }
        config = sim::load_simulator_config(*path);
        if (const auto v = args.value("--host")) config.host = *v;
        if (const auto v = args.number<std::uint16_t>("--port")) config.port = *v;
        if (const auto v = args.number<double>("--duration")) config.duration_s = *v;
        if (const auto v = args.number<std::uint64_t>("--seed")) config.seed = *v;
        if (const auto v = args.value("--log-level")) {
            const auto level = parse_log_level(*v);
            if (!level) cli::Args::fail("invalid --log-level " + *v);
            log_level = *level;
        }
        args.reject_unknown();
    } catch (const ConfigError& e) {
        std::cerr << "configuration error: " << e.what() << "\n";
        return 2;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    Logger logger(log_level);
    sim::Simulator simulator(config, logger);
    simulator.start();
    std::cout << "sensor_sim: " << simulator.sensor_count() << " sensors -> " << config.host << ":"
              << config.port << "\n"
              << std::flush;

    // A helper thread turns SIGINT/SIGTERM into simulator.stop(); the main thread waits
    // for either that or the configured duration.
    std::thread signal_thread([&] {
        int sig = 0;
        sigwait(&signals, &sig);
        simulator.stop();
    });

    const auto start = std::chrono::steady_clock::now();
    std::thread stats_thread;
    std::atomic<bool> done{false};
    if (config.stats_interval.count() > 0) {
        stats_thread = std::thread([&] {
            auto next = start + config.stats_interval;
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (std::chrono::steady_clock::now() >= next) {
                    next += config.stats_interval;
                    print_stats(simulator.stats(),
                                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                              start)
                                    .count());
                }
            }
        });
    }

    simulator.wait();
    simulator.stop();
    done.store(true);
    if (stats_thread.joinable()) {
        stats_thread.join();
    }
    // Wake the signal thread if we finished because the duration elapsed.
    pthread_kill(signal_thread.native_handle(), SIGTERM);
    signal_thread.join();

    std::cout << "final: ";
    print_stats(simulator.stats(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return 0;
}
