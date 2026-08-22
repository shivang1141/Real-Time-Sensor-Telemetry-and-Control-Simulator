// telemetry_server: central telemetry processing service.

#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <system_error>

#include "common/cli.hpp"
#include "telemetry/app/server_config.hpp"
#include "telemetry/app/status_reporter.hpp"
#include "telemetry/app/telemetry_service.hpp"

namespace {

constexpr const char* kUsage = R"(Usage: telemetry_server [options]

Options:
  --config <path>            JSON configuration file (default: built-in defaults)
  --port <n>                 Override network.port (0 = ephemeral)
  --workers <n>              Override processing.workers
  --queue-capacity <n>       Override processing.queue_capacity_per_worker
  --simulated-work-us <n>    Override processing.simulated_work_us (benchmarking)
  --log-level <level>        debug | info | warn | error | off
  --log-file <path>          Write logs to a file instead of stderr
  --status-interval-ms <n>   Status print interval; 0 disables
  --metrics-json <path>      Write final metrics as JSON on shutdown
  --quiet                    Do not print periodic status or the final summary
  --help                     Show this help

Stop with Ctrl+C (SIGINT) or SIGTERM for a graceful shutdown.
)";

timespec to_timespec(std::chrono::milliseconds ms) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ms.count() / 1000);
    ts.tv_nsec = static_cast<long>((ms.count() % 1000) * 1'000'000);
    return ts;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace telemetry;
    cli::Args args(argc, argv);
    if (args.flag("--help") || args.flag("-h")) {
        std::cout << kUsage;
        return 0;
    }

    ServerConfig config;
    std::optional<std::string> metrics_json_path;
    bool quiet = false;
    try {
        if (const auto path = args.value("--config")) {
            config = load_server_config(*path);
        }
        if (const auto v = args.number<std::uint16_t>("--port")) config.network.port = *v;
        if (const auto v = args.number<std::size_t>("--workers")) config.processing.workers = *v;
        if (const auto v = args.number<std::size_t>("--queue-capacity")) {
            config.processing.queue_capacity_per_worker = *v;
        }
        if (const auto v = args.number<std::uint32_t>("--simulated-work-us")) {
            config.processing.simulated_work = std::chrono::microseconds(*v);
        }
        if (const auto v = args.value("--log-level")) {
            const auto level = parse_log_level(*v);
            if (!level) cli::Args::fail("invalid --log-level " + *v);
            config.logging.level = *level;
        }
        if (const auto v = args.value("--log-file")) config.logging.file = *v;
        if (const auto v = args.number<std::uint32_t>("--status-interval-ms")) {
            config.status.interval = std::chrono::milliseconds(*v);
        }
        metrics_json_path = args.value("--metrics-json");
        quiet = args.flag("--quiet");
        args.reject_unknown();
        config.validate();
    } catch (const ConfigError& e) {
        std::cerr << "configuration error: " << e.what() << "\n";
        return 2;
    }

    Logger logger(config.logging.level);
    if (!config.logging.file.empty()) {
        try {
            logger.open_file(config.logging.file);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 2;
        }
    }

    // Block SIGINT/SIGTERM before any thread starts so every thread inherits the mask;
    // the main thread then receives them synchronously with sigtimedwait(). This avoids
    // doing any work inside an async signal handler.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    SystemClock clock;
    TelemetryService service(config, logger, clock);
    try {
        service.start();
    } catch (const std::system_error& e) {
        logger.error("main", "start_failed", {{"error", e.what()}});
        std::cerr << "failed to start: " << e.what() << "\n";
        return 1;
    }
    std::cout << "telemetry_server listening on port " << service.port() << " with "
              << config.processing.workers << " worker(s). Press Ctrl+C to stop.\n"
              << std::flush;

    const SteadyTime started = clock.steady_now();
    auto uptime = [&] {
        return std::chrono::duration<double>(clock.steady_now() - started).count();
    };
    const bool print_status = !quiet && config.status.interval.count() > 0;
    const timespec wait_time =
        to_timespec(print_status ? config.status.interval : std::chrono::milliseconds(1000));
    StatusSample previous = service.sample(false);

    while (true) {
        const int sig = sigtimedwait(&signals, nullptr, &wait_time);
        if (sig == SIGINT || sig == SIGTERM) {
            logger.info("main", "signal_received", {{"signal", strsignal(sig)}});
            break;
        }
        if (sig < 0 && errno != EAGAIN && errno != EINTR) {
            logger.error("main", "sigtimedwait_failed", {{"error", std::strerror(errno)}});
            break;
        }
        if (print_status) {
            StatusSample current = service.sample(true);
            std::cout << format_status(current, compute_interval(previous, current), uptime(),
                                       config.status.max_listed_sensors)
                      << std::flush;
            previous = std::move(current);
        }
    }

    service.stop();
    const StatusSample final_sample = service.sample(false);
    if (!quiet) {
        std::cout << format_final_summary(final_sample, uptime()) << std::flush;
    }
    if (metrics_json_path) {
        std::ofstream out(*metrics_json_path);
        out << metrics_to_json(final_sample, uptime()) << "\n";
        if (!out) {
            std::cerr << "failed to write " << *metrics_json_path << "\n";
            return 1;
        }
    }
    return 0;
}
