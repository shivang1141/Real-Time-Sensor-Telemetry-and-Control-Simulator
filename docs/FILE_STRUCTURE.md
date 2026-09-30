# File structure

This page lists every file in the repository: what it contains, what it is for, and how it
connects to the rest of the project. For a plain-language introduction to the project, see
[WHAT_IS_THIS_PROJECT.md](WHAT_IS_THIS_PROJECT.md).

## Contents

1. [The tree at a glance](#1-the-tree-at-a-glance)
2. [How the folders fit together](#2-how-the-folders-fit-together)
3. [Root files](#3-root-files)
4. [`include/telemetry/`: public headers](#4-includetelemetry-public-headers)
5. [`src/`: implementation](#5-src-implementation)
6. [`apps/`: the two programs](#6-apps-the-two-programs)
7. [`config/`: configuration files](#7-config-configuration-files)
8. [`tests/`: unit and integration tests](#8-tests-unit-and-integration-tests)
9. [`benchmarks/`: performance tools and results](#9-benchmarks-performance-tools-and-results)
10. [`scripts/`: automation](#10-scripts-automation)
11. [`docs/`: documentation and diagrams](#11-docs-documentation-and-diagrams)
12. [`visualizer/`: interactive browser demo](#12-visualizer-interactive-browser-demo)
13. [`cmake/`: build helpers](#13-cmake-build-helpers)
14. [`.github/`: CI and deployment](#14-github-ci-and-deployment)
15. [Generated folders (not in git)](#15-generated-folders-not-in-git)
16. [Where to look for…](#16-where-to-look-for)

---

## 1. The tree at a glance

```
.
├── CMakeLists.txt            top-level build definition
├── CMakePresets.json         debug / release / asan / tsan build presets
├── Dockerfile                build + test + runtime image
├── docker-compose.yml        server + simulator containers
├── README.md                 project landing page
├── .clang-format             code style
├── .dockerignore             files kept out of the Docker build
├── .gitattributes            line-ending rules
├── .gitignore                files kept out of git
│
├── include/telemetry/        PUBLIC HEADERS (the API of each layer)
│   ├── common/               clock, logger, the TelemetryMessage type
│   ├── protocol/             wire format, line framer
│   ├── net/                  sockets, TCP server, TCP client
│   ├── processing/           queue, workers, sensor state, anomaly rules, events
│   ├── metrics/              counters, latency histogram
│   ├── app/                  server config, service wiring, status output
│   └── sim/                  sensor signal model, simulator
│
├── src/                      IMPLEMENTATION (.cpp files, mirrors include/)
│   ├── common/  protocol/  net/  processing/  metrics/  app/  sim/
│   └── CMakeLists.txt        builds everything into the telemetry_core library
│
├── apps/                     THE TWO PROGRAMS (main functions)
│   ├── server/main.cpp       telemetry_server
│   ├── sensor_sim/main.cpp   sensor_sim
│   └── common/cli.hpp        shared command-line parsing
│
├── config/                   JSON configuration
│   ├── server.json
│   ├── simulator.json
│   └── scenarios/            faults, many_sensors, overload_server
│
├── tests/                    114 GoogleTest tests
│   ├── unit/                 92 tests, 10 files
│   ├── integration/          22 tests over real TCP, 3 files
│   └── support/              shared test helpers
│
├── benchmarks/               performance tools and recorded results
│   ├── bench_components.cpp  per-stage micro-benchmarks
│   ├── loadgen.cpp           high-rate load generator
│   └── results/              CSV + Markdown results, machine info
│
├── scripts/                  demo, benchmark runner, results summarizer
├── docs/                     write-ups and diagrams
├── visualizer/               interactive browser model (one HTML file)
├── cmake/                    compiler warnings, sanitizers, dependencies
└── .github/workflows/        CI pipeline, GitHub Pages deployment
```

---

## 2. How the folders fit together

```
                    ┌──────────────────────────────────────────────┐
config/*.json ───▶  │ apps/server/main.cpp      apps/sensor_sim/   │  the two programs
                    └───────────────┬──────────────────────────────┘
                                    │ link against
                    ┌───────────────▼──────────────────────────────┐
                    │ telemetry_core library = src/ + include/     │  all the real logic
                    │   app ─▶ net · protocol · processing · metrics│
                    │                    └──▶ common               │
                    │   sim ─▶ net · protocol · metrics · common   │
                    └───────────────▲──────────────────▲───────────┘
                                    │ link against     │ link against
                         tests/ (GoogleTest)     benchmarks/
```

* **`include/` + `src/`** together form one library, `telemetry_core`. Every header in
  `include/telemetry/<layer>/` has a matching `.cpp` in `src/<layer>/` (except header-only
  classes such as the queue and the framer).
* **`apps/`** contains only thin `main()` functions. They read flags and config, build the
  objects from `telemetry_core`, and handle signals.
* **`tests/`** and **`benchmarks/`** link the same library, so they test and measure the
  exact code the programs run.
* **Layers depend downward only.** `common` depends on nothing. `net` only moves bytes and
  never sees telemetry types. `processing` has no networking. `app` is the only place where
  networking and processing meet.

---

## 3. Root files

| File | What it is | Details |
|---|---|---|
| `README.md` | Project landing page | Overview, live demo link, architecture diagrams, quick start, example output, configuration, testing, performance, limitations. |
| `CMakeLists.txt` | Top-level build | C++17, no compiler extensions, exports `compile_commands.json`. Options: `TELEMETRY_BUILD_TESTS`, `TELEMETRY_BUILD_BENCHMARKS`, `TELEMETRY_WARNINGS_AS_ERRORS`, `TELEMETRY_SANITIZER` (`address` or `thread`). Adds `src`, `apps`, `tests`, `benchmarks`. |
| `CMakePresets.json` | Named build configurations | `debug` (warnings are errors), `release` (`-O3`, used for benchmarks), `asan` (AddressSanitizer + UndefinedBehaviorSanitizer), `tsan` (ThreadSanitizer). Each has build and test presets; test presets set sanitizer options and a 120 s timeout. All use Ninja and build into `build/<preset>`. |
| `Dockerfile` | Two-stage container build | **build** stage (Ubuntu 24.04): installs the toolchain, configures, builds and **runs the full test suite**, so the image only builds if every test passes. **runtime** stage: copies `telemetry_server`, `sensor_sim` and `telemetry_loadgen`, plus `config/`, runs as a non-root user, exposes port 9000. |
| `docker-compose.yml` | Two containers | `server` (port 9000, stops with SIGINT for a graceful shutdown) and `simulator` (connects to `server`). `SIM_CONFIG=scenarios/faults.json` selects a scenario. |
| `.clang-format` | Code style | Google style, 4-space indent, 100-column limit. |
| `.gitignore` | Ignored by git | `build/`, `out/`, IDE folders, `compile_commands.json`, logs, raw benchmark output. |
| `.gitattributes` | Line endings | Text files use LF; PNGs are binary. Keeps shell scripts working when checked out on Windows. |
| `.dockerignore` | Excluded from Docker build context | Build output, `.git`, IDE folders, raw benchmark data, the visualizer and the demo GIF. |

---

## 4. `include/telemetry/`: public headers

Headers declare the classes and functions of each layer. Most carry a doc comment that
explains the design choice behind them.

### `common/`: shared basics (depends on nothing)

| File | Contains |
|---|---|
| `telemetry_message.hpp` | `TelemetryMessage` (sensor id, type, value, timestamp, sequence number, status). `SensorType` (temperature, pressure, motor_speed, signal_strength) and `SensorStatus` (ok, degraded, fault) enums, with string conversion and units. |
| `clock.hpp` | `Clock` interface with a monotonic `steady_now()` for measuring durations and `wall_now_us()` for message timestamps. `SystemClock` for real use; `ManualClock` for tests, which advance time by hand so staleness and timestamp logic test deterministically. |
| `logger.hpp` | Thread-safe structured `Logger` writing logfmt lines (`time level=info component=net event=... key=value`). Passed by reference; there is no global logger. Also `RateLimiter`, which caps how often a flood condition is logged and counts the suppressed lines. |

### `protocol/`: the wire format

| File | Contains |
|---|---|
| `protocol.hpp` | Protocol definition: one JSON object per line (NDJSON). Required fields `id`, `type`, `value`, `ts`, `seq`; optional `status`. `encode()` and `decode()` (never throws; returns a `ParseError` such as `malformed_json`, `missing_field`, `non_finite_value`). `is_valid_sensor_id()`: 1 to 64 characters of `[A-Za-z0-9._-]`. |
| `line_framer.hpp` | `LineFramer` (header-only): splits a TCP byte stream into `\n`-terminated frames. Buffers at most `max_frame_bytes`, so memory per connection is bounded. Discards oversized frames and resynchronizes at the next newline. Uses a zero-copy fast path when a frame sits entirely in one read. |

### `net/`: networking (moves bytes, knows nothing about telemetry)

| File | Contains |
|---|---|
| `socket.hpp` | `FileDescriptor` (RAII, move-only, closes on destruction). `WakeupEvent` (an `eventfd` that wakes every polling thread at shutdown). Helpers: `listen_tcp`, connect with timeout, `send_all` (loops over partial writes and `EINTR`, uses `MSG_NOSIGNAL`), `peer_name`, `local_port`, send timeouts. |
| `tcp_server.hpp` | `TcpServer`: one acceptor thread plus one thread per connection, polling the socket and the shutdown eventfd. `ConnectionHandler` interface (`on_connect`, `on_frame`, `on_oversized_frame`, `on_disconnect`, `on_connection_rejected`). `TcpServerConfig` (bind address, port, backlog, max connections, frame size limit, buffer size, idle timeout). `DisconnectReason` enum. |
| `tcp_client.hpp` | `TcpClient`: blocking client used by the simulator, the load generator and tests. `send_line` (frame + `\n` in one write), `send_raw` (no framing, used to test partial frames). |

### `processing/`: the concurrent pipeline and detection logic

| File | Contains |
|---|---|
| `bounded_queue.hpp` | `BoundedQueue<T>` (header-only): fixed-capacity multi-producer/multi-consumer FIFO built on a mutex and two condition variables. `try_push` fails fast when full (the server's drop policy) using an atomic size hint, so it doesn't take the lock. `pop_batch` drains many items per lock. `close()` lets consumers drain what is left. Tracks a high-water mark. |
| `processing_engine.hpp` | `ProcessingEngine`: N shards, each with one queue, one worker thread and one `SensorRegistry`. `submit()` routes by `hash(sensor_id) % N` and never blocks (`Accepted`, `QueueFull`, `ShuttingDown`). `ProcessingConfig` (workers, queue capacity, stale-check interval, simulated work). |
| `sensor_registry.hpp` | `SensorRegistry`: per-shard state for each sensor (last message, trend window, active conditions, counters). `update()` validates a message, checks for sequence gaps, updates the window, runs detection and emits events. `check_stale()` marks silent sensors. `SensorHealth` (ok, warning, critical, fault, stale), `SensorSnapshot`, `HealthSummary`. |
| `anomaly_detector.hpp` | `Thresholds` per sensor type (valid range, warn/crit low/high, hysteresis, max rate, max trend). `DetectorConfig` (stale timeout, trend window, rate interval, clock-skew limits). `AnomalyDetector`: stateless rules, with `validate()` (physical range, timestamps, order) and `evaluate()` (limits with hysteresis, rate of change, least-squares trend, sensor-reported status). |
| `events.hpp` | `Condition` bit flags (`warn_low`, `warn_high`, `crit_low`, `crit_high`, `rate_of_change`, `trend`, `sensor_degraded`, `sensor_fault`, `stale`). `Severity`, `RejectReason`, `EventKind`, `Event`. `EventSink` interface and `LoggingEventSink`, which counts every event and rate-limits logging. |

### `metrics/`: measurements

| File | Contains |
|---|---|
| `metrics.hpp` | `Metrics`: process-wide counters (connections, frames received, invalid, oversized, enqueued, dropped, processed, rejected, sequence missing) as relaxed atomics, plus latency histograms. `MetricsSnapshot` copies them for reporting. Documents the counter invariants. |
| `latency_histogram.hpp` | `LatencyHistogram`: fixed-size log-linear histogram in microseconds (exact below 64 µs, about 3% relative error above). `record()` is wait-free. `HistogramSnapshot` gives percentiles and mean and can be subtracted to get the last interval only. |

### `app/`: putting the server together

| File | Contains |
|---|---|
| `server_config.hpp` | `ServerConfig` (network, processing, detection, logging, status settings) with `validate()`. `load_server_config` / `parse_server_config` read JSON where every field is optional and unknown fields are errors. |
| `telemetry_service.hpp` | `TelemetryService`: the server itself. Implements `ConnectionHandler`: decodes each frame on the network thread, then submits it to the engine. `start()` starts workers, then the listener. `stop()` stops the listener first, then drains the queues. `StatusSample` captures everything needed for a report. |
| `status_reporter.hpp` | Pure formatting functions: `compute_interval` (rates and interval latency between two samples), `format_status` (the periodic terminal block), `format_final_summary` (printed at exit), `metrics_to_json` (machine-readable output for benchmark scripts). |
| `config_error.hpp` | `ConfigError` exception for unreadable or invalid configuration. |

### `sim/`: the sensor simulator

| File | Contains |
|---|---|
| `sensor_model.hpp` | `SensorSpec` (id, type, rate, nominal value, noise, drift) and `FaultSpec` (spike, drop, malformed and delay probabilities, ramp, failure window, `FailMode`: silent, disconnect, fault_status). `SensorModel`: deterministic signal generator. The same seed always gives the same values and faults. |
| `simulator.hpp` | `SimulatorConfig` (server address, seed, duration, sensor list, reconnect timings) and its JSON loader, which expands groups into sensors named `<prefix>-001`, `-002`, …. `Simulator` runs one thread per sensor on absolute deadlines, with reconnect and exponential backoff. `SimulatorStats` counters. |

---

## 5. `src/`: implementation

Every `.cpp` here implements the header with the same name. `src/CMakeLists.txt` compiles all
of them into the **`telemetry_core`** library, with the project's strict warning set and
`nlohmann_json` as a private dependency.

| File | Implements | Notable details |
|---|---|---|
| `common/telemetry_message.cpp` | Enum ↔ string conversion, units (`degC`, `kPa`, `rpm`, `dBm`) | |
| `common/logger.cpp` | logfmt formatting, quoting and escaping, file output, `RateLimiter` | Formats outside the lock; only the write is serialized. |
| `common/json_reader.hpp` | `ObjectReader` (internal, not public API) | Strict config reading: type checks, defaults, exact error paths like `processing.workers: expected unsigned integer`, and rejects unknown keys. |
| `common/json_reader.cpp` | Out-of-line part of the reader | |
| `protocol/protocol.cpp` | `encode`, `decode`, sensor id rules | Uses nlohmann::json. Decoding is the largest CPU cost in the pipeline (see PERFORMANCE.md). |
| `net/socket.cpp` | Sockets, `eventfd`, connect with timeout, `send_all` | All POSIX calls are wrapped and errors are reported as `std::system_error`. |
| `net/tcp_server.cpp` | Acceptor loop, per-connection loop, connection limit, idle timeout, shutdown | Backs off when out of file descriptors instead of spinning. Never holds two mutexes at once. |
| `net/tcp_client.cpp` | Client connect and send | |
| `processing/events.cpp` | Enum strings, severities, `LoggingEventSink` | Rate limits: 20 lines/s for per-message events, 200 lines/s for transitions. |
| `processing/anomaly_detector.cpp` | Default thresholds, `validate`, `evaluate`, `trend_slope` | Centers timestamps before the least-squares sums to keep precision. |
| `processing/sensor_registry.cpp` | Per-sensor update, sequence gaps, restarts, staleness, edge-triggered events, snapshots | A sequence number of 0 after progress is treated as a sensor restart. |
| `processing/processing_engine.cpp` | Sharding, `submit`, worker loop (`pop_batch(64)`), latency recording, stale checks | |
| `metrics/metrics.cpp` | `Metrics::snapshot()` | |
| `metrics/latency_histogram.cpp` | Bucket math, percentiles, interval subtraction | |
| `app/server_config.cpp` | Server JSON loading and validation | |
| `app/telemetry_service.cpp` | Wires `TcpServer` to `ProcessingEngine`; counts invalid, oversized and dropped frames | |
| `app/status_reporter.cpp` | Status block, final summary, metrics JSON | |
| `sim/sensor_model.cpp` | Signal generation and fault injection, including four kinds of corrupted frames | |
| `sim/simulator.cpp` | Sensor threads, absolute-deadline timing, reconnect with backoff, counters | Per-sensor seeds are derived with a SplitMix64 mix. |
| `sim/simulator_config.cpp` | Simulator JSON loading and group expansion | |

---

## 6. `apps/`: the two programs

| File | Program | What it does |
|---|---|---|
| `server/main.cpp` | **`telemetry_server`** | Loads `--config`, applies flag overrides (`--port`, `--workers`, `--queue-capacity`, `--simulated-work-us`, `--log-level`, `--log-file`, `--status-interval-ms`, `--metrics-json`, `--quiet`). Blocks SIGINT/SIGTERM before starting any thread, then waits for them with `sigtimedwait` while printing the status block. On a signal it shuts down gracefully and prints a final summary. |
| `sensor_sim/main.cpp` | **`sensor_sim`** | Loads `--config` (required) with overrides `--host`, `--port`, `--duration`, `--seed`, `--log-level`. Starts the simulator. Prints generated, sent, dropped, malformed and reconnect counts periodically (`stats_interval_ms`) and once more at exit. A helper thread turns SIGINT/SIGTERM into a clean stop. |
| `common/cli.hpp` | shared | Minimal `--name value` / `--flag` parser with typed numbers; rejects unknown options. |
| `CMakeLists.txt` | build | Two executables linked to `telemetry_core`. |

---

## 7. `config/`: configuration files

All files are JSON with comments allowed. Unknown keys are rejected with an exact path, so
typos fail loudly.

| File | Used by | Contents |
|---|---|---|
| `server.json` | server | Every server setting, documented by example: network limits (port 9000, max 256 connections, 1024-byte frames, 30 s idle timeout), 4 workers × 4096 queue slots, detection timing (stale after 2 s, trend window 20) and thresholds for all four sensor types. |
| `simulator.json` | simulator | Healthy fleet: 12 sensors (4 temperature, 3 pressure, 3 motor, 2 signal) with noise and slow drift, no faults. Runs until stopped. |
| `scenarios/faults.json` | simulator | 40-second fault showcase with one group per failure: healthy reference, spikes, overheating ramp, silent sensor, disconnecting sensor, fault status, lossy link. |
| `scenarios/many_sensors.json` | simulator | Scale test: 200 sensors at 50 Hz on 200 connections, about 10,000 messages per second. |
| `scenarios/overload_server.json` | server | Deliberately under-provisioned server (1 worker, 200 µs work per message, 1024 queue slots). Paired with `many_sensors.json` to show queue saturation and the drop policy. |

---

## 8. `tests/`: unit and integration tests

**114 tests** in total, using GoogleTest. They run under Debug, Release, ASan+UBSan and TSan.
`tests/CMakeLists.txt` builds two executables, `unit_tests` and `integration_tests`, and
registers each test with CTest.

### `tests/unit/`: 92 tests, no network

| File | Tests | What it checks |
|---|---:|---|
| `test_anomaly_detector.cpp` | 15 | Threshold ordering, warning/critical limits, boundaries, hysteresis (no flapping), sensor status, rate of change (reference sample choice), trend minimum samples and least-squares slope, rejecting implausible values, bad timestamps and out-of-order messages. |
| `test_bounded_queue.cpp` | 14 | FIFO order, full-queue behavior, high-water mark, timeouts, close and drain, waking blocked threads, `pop_batch`, and multi-threaded stress tests (capacity never exceeded, every item delivered exactly once). |
| `test_protocol.cpp` | 11 | Encode/decode round trip for all sensor types, optional status, unknown fields ignored, structural errors, missing fields, wrong types, invalid and non-finite values, sensor id rules. |
| `test_sensor_registry.cpp` | 11 | Registration, latest reading, edge-triggered events, health priority, sequence gaps, duplicate and out-of-range rejection, restart detection, stale detection and recovery, and that rejected messages don't refresh freshness. |
| `test_sensor_model.cpp` | 9 | Noise-free output, same seed gives the same output, noise spread, ramp timing, failure window, fault status, drop and malformed probabilities, corrupted frames never decode, delay range. |
| `test_config.cpp` | 8 | Server and simulator config: defaults, overrides, unknown fields, wrong types, group expansion, and that every shipped config file loads. |
| `test_latency_histogram.cpp` | 8 | Exact small values, bucket bounds, clamping, percentile error bound, interval subtraction, concurrent recording loses nothing. |
| `test_line_framer.cpp` | 7 | Several frames in one read, frames split across reads, byte-at-a-time delivery, `\r` and empty lines, frame exactly at the limit, oversized frames discarded with bounded memory. |
| `test_logger.cpp` | 5 | logfmt format, quoting and escaping, level filtering, level parsing, rate limiter. |
| `test_status_reporter.cpp` | 4 | Interval rates, zero elapsed time, only unhealthy sensors listed, metrics JSON complete and parseable. |

### `tests/integration/`: 22 tests over real TCP on localhost

| File | Tests | What it checks |
|---|---:|---|
| `test_telemetry_service.cpp` | 10 | Full server end to end: one and many sensors, partial TCP writes reassembled, invalid and oversized messages counted while the connection survives, semantic rejection, disconnect mid-frame, **queue saturation with every frame accounted for**, **graceful shutdown drains queued messages**, prompt shutdown with idle clients, anomaly and stale detection. |
| `test_tcp_server.cpp` | 9 | One client, byte-by-byte frames, many concurrent clients keep per-connection order, oversized frames, client disconnect, idle timeout, connection limit, prompt stop, port already in use. |
| `test_simulator.cpp` | 3 | Simulator streams to a real server; disconnect failure reconnects; simulator waits and connects when the server starts late. |

### `tests/support/`

| File | Contains |
|---|---|
| `test_helpers.hpp` | `wait_until(predicate, timeout)`: polls instead of sleeping for a fixed time, so tests are fast when things work and still pass on slow sanitizer builds. `make_message(...)` builds a test `TelemetryMessage`; `now_us()` gives the current wall-clock time. |

---

## 9. `benchmarks/`: performance tools and results

| File | What it is |
|---|---|
| `bench_components.cpp` | Micro-benchmarks for each stage per message: decode, encode, `SensorRegistry::update`, `LineFramer`, histogram record, queue push+pop. Shows where time is spent. |
| `loadgen.cpp` | **`telemetry_loadgen`**: high-rate load generator. Unlike `sensor_sim`, it puts many sensors on a few connections and batches frames into large writes, so the generator is never the bottleneck. Flags: `--connections`, `--sensors-per-connection`, `--rate`, `--duration`, `--batch`. Prints a JSON summary. |
| `CMakeLists.txt` | Builds `bench_components` and `telemetry_loadgen`. |
| `results/results.csv` | Raw pipeline benchmark results: 10 cases × 3 runs, with throughput, drops, p50/p95/p99 latency, CPU and memory. |
| `results/summary.md` | Median-of-3 table generated from `results.csv`. |
| `results/before_optimization_5s.csv` | Results from before the queue optimization, kept for the before/after comparison. |
| `results/machine.json` | CPU, core count, RAM, kernel and compiler of the benchmark machine. |

---

## 10. `scripts/`: automation

| File | What it does |
|---|---|
| `demo.sh` | One-command demo (about 40 s). Builds if needed, starts the server, runs the fault scenario, shuts down gracefully and summarizes what was detected. Usage: `scripts/demo.sh [build_dir] [scenario]`. |
| `run_benchmarks.sh` | Runs `telemetry_server` and `telemetry_loadgen` as separate processes for every benchmark case. Records throughput, latency, drops, CPU (from `/proc/<pid>/stat`) and peak memory (`VmHWM`). Configurable with `DURATION`, `REPEATS` and `CASES`. |
| `summarize_benchmarks.py` | Turns `results.csv` into the Markdown table in `summary.md` (median per case, min–max range). |

---

## 11. `docs/`: documentation and diagrams

| File | What it is |
|---|---|
| `WHAT_IS_THIS_PROJECT.md` | Plain-language introduction: what the project is, why it exists, every technology used. |
| `FILE_STRUCTURE.md` | This page. |
| `ARCHITECTURE.md` | Full design: components, data flow of one message, thread model, synchronization inventory, wire protocol, validation and detection rules, failure behavior, shutdown sequence, time handling, limitations. |
| `METRICS.md` | Definition of every counter, gauge, rate and latency, and the invariants between them. |
| `TESTING.md` | How to run the tests, what each suite covers, test design principles, how to read failures. |
| `PERFORMANCE.md` | Benchmark environment, method, per-stage costs, pipeline results, the queue optimization (before/after), `perf` profile, and what is not yet optimized. |
| `system-overview.svg` / `system-overview-dark.svg` | Simple "How it works" diagram for the README (light and dark versions). |
| `architecture.svg` / `architecture-dark.svg` | Detailed design diagram: every thread, queue and source file. |
| `visualizer-demo.gif` | 12-second recording of the visualizer, shown at the top of the README. |

---

## 12. `visualizer/`: interactive browser demo

| File | What it is |
|---|---|
| `index.html` | A self-contained page (HTML, CSS and JavaScript; no libraries, no build step) that animates the pipeline one message at a time. It includes a JavaScript port of the sensor model, registry and anomaly rules, three scenarios, step-by-step **Trace a message**, live **Inject fault**, metrics charts, and a status console in the server's format. Published to GitHub Pages by `.github/workflows/pages.yml`; also works when opened directly from disk. |

---

## 13. `cmake/`: build helpers

| File | What it does |
|---|---|
| `CompilerOptions.cmake` | Defines `telemetry_warnings`: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wformat=2`, plus `-Werror` when enabled. Applies sanitizer flags to the whole build when `TELEMETRY_SANITIZER` is set. |
| `Dependencies.cmake` | Uses the system nlohmann_json (≥ 3.10) and GoogleTest (≥ 1.10) when installed; otherwise downloads pinned versions (v3.11.3 and v1.14.0) with `FetchContent`. |

---

## 14. `.github/`: CI and deployment

| File | What it does |
|---|---|
| `workflows/ci.yml` | Runs on every push to `main` and every pull request. **build-and-test**: {gcc, clang} × {debug, release}, configure, build and run all tests. **sanitizers**: clang with `asan` and `tsan`. **docker**: builds the image (which runs the tests) and smoke-tests both programs. |
| `workflows/pages.yml` | When `visualizer/` changes on `main`, publishes it to GitHub Pages. One-time setup: Settings → Pages → Source: GitHub Actions. |

---

## 15. Generated folders (not in git)

| Folder | Created by | Contains |
|---|---|---|
| `build/<preset>/` | `cmake --preset <name>` | Compiled programs (`apps/telemetry_server`, `apps/sensor_sim`), `benchmarks/telemetry_loadgen`, `benchmarks/bench_components`, `tests/unit_tests`, `tests/integration_tests`, and downloaded dependencies in `_deps/`. |
| `benchmarks/results/raw/` | `run_benchmarks.sh` | Per-run JSON and logs behind `results.csv`. |

---

## 16. Where to look for…

| If you want to see… | Look at |
|---|---|
| How a message travels through the server | `src/app/telemetry_service.cpp` → `src/processing/processing_engine.cpp` → `src/processing/sensor_registry.cpp` |
| The alarm rules | `src/processing/anomaly_detector.cpp` |
| TCP handling, threads and shutdown wake-up | `src/net/tcp_server.cpp`, `src/net/socket.cpp` |
| Splitting a byte stream into messages | `include/telemetry/protocol/line_framer.hpp` |
| The bounded queue and its optimization | `include/telemetry/processing/bounded_queue.hpp`, `docs/PERFORMANCE.md` |
| Signal handling and graceful shutdown | `apps/server/main.cpp`, `TelemetryService::stop()` |
| How faults are simulated | `src/sim/sensor_model.cpp`, `config/scenarios/faults.json` |
| Proof that it works under stress | `tests/integration/test_telemetry_service.cpp`, `tests/unit/test_bounded_queue.cpp` |
| Performance numbers | `benchmarks/results/summary.md`, `docs/PERFORMANCE.md` |
