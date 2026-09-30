# What is this project?

## In one sentence

A **C++17 system for Linux in which a fleet of simulated sensors streams readings over the
network to a multi-threaded server, which checks every reading, detects problems (values
out of range, overheating trends, sensors that fail or go silent) and reports live health,
throughput and latency.**

**[▶ See it running in your browser](https://shivang1141.github.io/Real-Time-Sensor-Telemetry-and-Control-Simulator/)**:
no install needed.

---

## Contents

1. [The idea, in plain words](#1-the-idea-in-plain-words)
2. [Why this kind of system matters](#2-why-this-kind-of-system-matters)
3. [What it does, step by step](#3-what-it-does-step-by-step)
4. [The two programs](#4-the-two-programs)
5. [What the server detects](#5-what-the-server-detects)
6. [What can go wrong, and how the system copes](#6-what-can-go-wrong-and-how-the-system-copes)
7. [Technology used](#7-technology-used)
8. [Engineering skills it demonstrates](#8-engineering-skills-it-demonstrates)
9. [Testing and quality](#9-testing-and-quality)
10. [Performance](#10-performance)
11. [Scenarios you can run](#11-scenarios-you-can-run)
12. [How to run it](#12-how-to-run-it)
13. [The interactive visualizer](#13-the-interactive-visualizer)
14. [Scope and honest limitations](#14-scope-and-honest-limitations)
15. [What could come next](#15-what-could-come-next)
16. [Glossary](#16-glossary)

---

## 1. The idea, in plain words

Imagine a factory, a vehicle or a power station full of sensors: thermometers, pressure
gauges, motor speed sensors, radio signal meters. Each one reports a number many times a
second. Somewhere, a computer has to:

* **receive** all of those readings at once, from many sensors,
* **check** that each reading makes sense,
* **notice** when something is wrong, such as a temperature climbing, a sensor that has
  stopped talking, or a sensor reporting that it is broken,
* **keep going** when data arrives damaged or too fast, and
* **report** what is happening, continuously.

This project builds both sides of that picture:

* a **simulator** that pretends to be the sensors, including realistic faults, and
* a **server** that receives, checks, analyzes and reports on everything they send.

All data is synthetic. No real hardware is involved.

---

## 2. Why this kind of system matters

Telemetry (measurements sent from a remote device) is everywhere: industrial machines,
robots, cars, drones, satellites, data centres, medical devices, smart buildings. The
software that collects it has to deal with the same hard problems in every domain:

| Problem | Why it is hard |
|---|---|
| Many sources at once | Needs concurrency without race conditions. |
| The network splits and merges data | TCP delivers bytes, not messages, so messages must be reassembled correctly. |
| Bad or malicious input | Must be rejected without crashing or using unbounded memory. |
| Bursts of traffic | When work arrives faster than it can be done, the system must stay responsive instead of freezing or running out of memory. |
| Devices fail in different ways | A sensor can send wrong values, stop sending, disconnect, or report its own fault. Each looks different and needs a different check. |
| You need to know what is happening | Throughput, latency and health have to be measured continuously. |
| Shutting down safely | Data already accepted should not be lost when the program stops. |

This project tackles each of these on purpose, in a small enough codebase to read end to
end.

---

## 3. What it does, step by step

```
 Sensors  ──TCP──▶  Receive  ──▶  Route  ──▶  Analyze  ──▶  Report
 (simulator)        & check       to a queue   (workers)     alarms, metrics, status
```

1. **Sensors send readings.** Each simulated sensor runs on its own thread and sends a
   reading 5 to 50 times a second, as one line of JSON:
   ```json
   {"id":"temp-001","seq":42,"status":"ok","ts":1729500000123456,"type":"temperature","value":21.5}
   ```
2. **The server receives and checks.** Each network connection has its own thread. It reads
   raw bytes, splits them into complete messages, and parses and checks each one. Broken
   messages are counted and thrown away at this point, before they can use any queue
   space.
3. **Messages are routed.** Every message goes into one of several queues, chosen by a hash
   of the sensor's id. A given sensor always lands in the same queue, so its readings stay in
   order and only one worker thread ever touches that sensor's state. Workers never share
   sensor data with each other.
4. **Workers analyze.** One worker thread per queue takes messages in batches of up to 64
   and checks the value against limits, the trend over the last 20 readings, the rate of
   change, missing sequence numbers, and the sensor's own status. Alarms are raised and
   cleared only when something *changes*, so the log is not flooded.
5. **Results are reported.** Every event goes to a structured log. Counters and latency
   histograms are updated by every thread, and once a second the server prints a status
   block:
   ```
   ---- status t=20.0s ----
     connections   active=9 accepted=10 rejected=0
     messages      recv=2746 (116/s) processed=2714 (115/s)
     errors        invalid=32 oversized=0 rejected=1 dropped=0 seq_missing=103
     queue         depth=0/16384 peak_shard=3 workers=4
     latency(us)   interval n=574 mean=166.4 p50=159 p95=271 p99=359 max=487
     sensors       total=9 ok=6 warning=2 critical=0 fault=1 stale=0 | active warnings=3 critical=1
       motor-fault-001      motor_speed      fault     value=2978.929   age=1ms [sensor_fault]
       rssi-lossy-002       signal_strength  warning   value=-61.339    age=23ms [rate_of_change]
       temp-ramp-001        temperature      warning   value=94.478     age=1ms [warn_high,trend]
   ```

A reading crosses the server in about **160 microseconds** (median).

---

## 4. The two programs

| | `sensor_sim` | `telemetry_server` |
|---|---|---|
| **Role** | Pretends to be a fleet of sensors | Receives and analyzes everything |
| **Threads** | One per sensor | One acceptor, one per connection, N workers, plus the main thread |
| **Input** | A JSON config listing sensor groups and faults | A JSON config with network limits, worker count and alarm thresholds |
| **Output** | Network traffic, plus its own statistics | Structured log, status block every second, final summary, optional metrics JSON |
| **Notable behavior** | Sends on exact schedules (absolute deadlines); reconnects with exponential backoff if the server goes away | Never blocks on a slow consumer; drains every queued message on shutdown |

A third program, **`telemetry_loadgen`**, is a high-speed traffic generator used only for
benchmarks.

---

## 5. What the server detects

Four sensor types are supported, each with its own limits (from `config/server.json`):

| Sensor type | Unit | Normal range | Warning above | Critical above |
|---|---|---|---|---|
| temperature | °C | 0 to 85 | 85 | 105 |
| pressure | kPa | 90 to 115 | 115 | 130 |
| motor_speed | rpm | 1,000 to 6,000 | 6,000 | 7,500 |
| signal_strength | dBm | −85 to −20 | −20 | −10 |

Checks, in the order a worker runs them:

| Check | What it catches |
|---|---|
| **Physical plausibility** | Values that cannot be real, such as −95 °C from a room sensor. These are rejected as bad data rather than raised as alarms. |
| **Timestamp and order** | Messages from the future, too old, or out of order. |
| **Sequence gaps** | Lost messages. Every sensor numbers its messages, so a jump from 41 to 44 means 2 were lost. |
| **Warning / critical limits** | Values outside the allowed range, low or high. |
| **Hysteresis** | Stops an alarm flickering on and off when a value hovers right at the limit: once raised, it clears only when the value is clearly back inside. |
| **Rate of change** | Values jumping too fast, measured over half a second. |
| **Trend** | A steady climb or fall (least-squares slope over the last 20 readings). Catches overheating *before* the limit is reached. |
| **Sensor-reported fault** | The sensor itself says `degraded` or `fault`. |
| **Stale** | No valid reading for 2 seconds: the sensor went silent or disconnected. |

Each sensor's health is then **ok**, **warning**, **critical**, **fault** or **stale**.

---

## 6. What can go wrong, and how the system copes

| Situation | What happens |
|---|---|
| A message arrives broken (bad JSON, missing fields) | Rejected at decode on the network thread, counted as `invalid`, logged with a rate limit. It never reaches a queue. |
| A message is huge | The framer never buffers more than 1,024 bytes. The rest is discarded until the next line, counted as `oversized`, and the connection keeps working. |
| A message is split across network packets, or several arrive together | The framer reassembles exactly; tested down to one byte at a time. |
| More data arrives than workers can handle | Queues have fixed capacity. When full, the **newest** message is dropped and counted (`dropped`). Network threads never block and memory stays flat. |
| A sensor disconnects | The server notices and closes that connection. The sensor reconnects with backoff, and the server sees the gap in sequence numbers. |
| A sensor goes silent but stays connected | Marked **stale** after 2 seconds; cleared when data returns. |
| A client connects and sends nothing | Closed after the idle timeout (30 s). |
| Too many clients | Connections beyond the limit are accepted and closed at once, so the client gets a clear failure instead of hanging. |
| Ctrl+C / SIGTERM | Stop accepting, close connections, **finish every message already queued**, join all threads, print a final summary. Nothing accepted is lost. |
| A config file has a typo | Rejected at startup with the exact path, for example `network.prot: unknown field`. |

---

## 7. Technology used

### Language and standard library

| Technology | Used for |
|---|---|
| **C++17** | The whole system: server, simulator, load generator, tests, benchmarks. |
| `std::thread` | Every thread: sensors, acceptor, connections, workers. |
| `std::mutex`, `std::condition_variable` | The bounded queue (one mutex, two condition variables) and protecting shared state. |
| `std::atomic` (relaxed ordering) | Wait-free metrics counters and latency histograms. |
| `std::chrono` (`steady_clock`, `system_clock`) | Monotonic time for durations and latency; wall-clock time for message timestamps. |
| `<random>` (`std::mt19937_64`, distributions) | Deterministic, seeded sensor noise and fault injection. |
| RAII, move-only types, `std::optional`, `std::string_view` | Safe ownership of sockets and file descriptors, zero-copy parsing paths, explicit "maybe" results. |

### Linux and POSIX system programming

| API | Used for |
|---|---|
| **POSIX sockets**: `socket`, `bind`, `listen`, `accept4`, `connect`, `recv`, `send` | TCP networking. |
| `poll` | Waiting for data or shutdown on each thread, with timeouts. |
| `eventfd` | One shutdown signal that wakes every waiting thread at once. |
| `sigtimedwait` + `pthread_sigmask` | Handling Ctrl+C / SIGTERM synchronously on the main thread, with no logic in async signal handlers. |
| `MSG_NOSIGNAL`, ignoring `SIGPIPE` | A closed peer becomes an error code instead of killing the process. |
| `TCP_NODELAY`, `SO_SNDTIMEO`, non-blocking connect | Low-latency sends, send timeouts, connect timeouts. |
| `/proc/<pid>/stat`, `VmHWM` | Measuring server CPU and peak memory in benchmarks. |

### Libraries

| Library | Used for |
|---|---|
| **nlohmann/json** (v3.11) | Parsing and writing the wire protocol and config files. |
| **GoogleTest** (v1.14) | Unit and integration tests. |

Both are taken from the system if installed (`apt`), otherwise downloaded at pinned
versions by CMake. There are no other runtime dependencies.

### Build and tooling

| Tool | Used for |
|---|---|
| **CMake** (≥ 3.22) with **presets** | Build definition; one command per configuration (`debug`, `release`, `asan`, `tsan`). |
| **Ninja** | Fast builds. |
| **GCC** (≥ 11) and **Clang** (≥ 14) | Both compilers are tested in CI. |
| Strict warnings | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast …`; warnings are errors in Debug. |
| **AddressSanitizer + UndefinedBehaviorSanitizer** | Detect memory errors, leaks and undefined behavior during tests. |
| **ThreadSanitizer** | Detect data races during tests. |
| **clang-format** | Consistent code style (Google-based, 4 spaces, 100 columns). |
| **perf** | CPU profiling to find where time is spent. |

### Testing, CI and delivery

| Technology | Used for |
|---|---|
| **CTest** | Runs the 114 tests in every build configuration. |
| **GitHub Actions** | CI on every push to `main` and every pull request: gcc and clang × Debug and Release, ASan+UBSan, TSan, and a Docker build. |
| **Docker** (multi-stage) | Reproducible build that runs the full test suite, plus a slim non-root runtime image. |
| **Docker Compose** | Runs the server and the simulator as two containers. |
| **GitHub Pages** | Hosts the interactive visualizer. |

### Scripting, data and documentation

| Technology | Used for |
|---|---|
| **Bash** | `demo.sh` (one-command demo) and `run_benchmarks.sh` (benchmark harness). |
| **Python 3** | `summarize_benchmarks.py` turns raw CSV into the results table. |
| **JSON** (with comments) | Configuration for the server, the simulator and the scenarios. |
| **NDJSON** | Wire protocol: one JSON object per line. |
| **logfmt** | Structured log format (`key=value` pairs) that is both readable and machine-parsable. |
| **CSV / Markdown** | Recorded benchmark results and documentation. |
| **SVG** | Architecture diagrams with light and dark variants. |

### Visualizer

| Technology | Used for |
|---|---|
| **HTML, CSS, JavaScript** (no frameworks, no build step) | The interactive browser model in `visualizer/index.html`. |
| **Canvas 2D** | Animating hundreds of messages per second through the pipeline. |
| **SVG** | Sparklines, throughput chart, latency histogram. |
| **Chrome DevTools Protocol** (headless Chrome) | Rendering exact frames to record the demo GIF. |

---

## 8. Engineering skills it demonstrates

| Area | Where it shows |
|---|---|
| **Linux systems programming** | Sockets, `poll`, `eventfd`, `accept4`, synchronous signals, RAII file descriptors. |
| **Network programming** | Multi-client TCP server; message framing over a byte stream; partial reads and writes; bounded memory per connection; connection limits; idle timeouts; reconnect with exponential backoff. |
| **Concurrency** | Producer–consumer pipeline; sharding to keep per-sensor order without locks on the hot path; bounded queues with an explicit overload policy; wait-free metrics; clean multi-thread shutdown; verified race-free with ThreadSanitizer. |
| **Algorithms** | Hysteresis thresholds, windowed rate of change, least-squares trend, stale detection, sequence-gap detection, log-linear latency histogram with bounded percentile error. |
| **Simulation** | Deterministic seeded signal models with noise, drift and eight kinds of injected faults (spike, ramp, drop, corrupt, delay, silent, disconnect, fault status). |
| **Software design** | Layered architecture where dependencies point down only; the detection logic has no I/O and no clock, so it is fully unit-testable; interfaces for clock, events and connections. |
| **Testing** | 92 unit and 22 integration tests over real TCP; multi-threaded stress tests; injected clocks; tests run under four build configurations. |
| **Performance engineering** | Separate-process load generator, repeatable benchmark harness, per-stage micro-benchmarks, `perf` profiling, and a measured optimization. |
| **DevOps** | CMake presets, CI matrix, sanitizers, Docker multi-stage build, Compose, GitHub Pages. |
| **Communication** | Architecture, metrics, testing and performance write-ups; diagrams; an interactive visualizer. |

---

## 9. Testing and quality

* **114 tests**: 92 unit tests and 22 integration tests that use real TCP connections on
  localhost.
* Every test runs under **four configurations**: Debug (warnings are errors), Release,
  ASan+UBSan and TSan.
* Integration tests cover the hard cases: messages sent one byte at a time, random
  fragmentation, oversized and malformed input, disconnects mid-message, idle timeouts,
  connection limits, queue saturation **with every message accounted for**, and graceful
  shutdown that drains the queues.
* The integration tests were run 20 times in a row (8 times under sanitizers) in parallel
  without a failure.
* Tests use an injected **manual clock** and poll for conditions instead of sleeping for a
  fixed time, so they are fast and not flaky.

Details: [TESTING.md](TESTING.md).

---

## 10. Performance

Measured on a laptop (Intel i5-1245U, WSL2), with the load generator on the same machine.
Each figure is the median of 3 × 10-second runs.

| Scenario | Result |
|---|---|
| Normal load, 20,000 messages/s | 0 dropped; latency p50 287 µs, p99 1.1 ms |
| Unbatched sends, 10,000 messages/s | p50 115 µs |
| Maximum throughput, 2 workers | about 381,000 messages/s |
| Worker scaling with 20 µs of work per message | 1 → 8 workers: 24.9k → 124k messages/s (5.0×) |
| 2× overload | processes 90% of theoretical capacity, drops and counts the rest; memory flat at about 4 MB |
| Queue optimization, found by benchmarking | 1.8–3.3× more throughput, p50 latency down 94% |

Profiling shows JSON parsing is now the largest cost (about 25–30% of CPU), about 14× the
cost of the anomaly detection itself.

Details: [PERFORMANCE.md](PERFORMANCE.md).

---

## 11. Scenarios you can run

| Scenario | Files | What you will see |
|---|---|---|
| **Healthy fleet** | `config/simulator.json` | 12 sensors with noise and slow drift; everything ok. |
| **Fault showcase** (40 s) | `config/scenarios/faults.json` | Random spikes; a sensor overheating from 8 s (trend alarm, then warning, then critical); a sensor silent from 10–18 s (stale); a sensor disconnecting from 12–18 s (stale, reconnect); a sensor reporting a fault from 15 s; a lossy link with 10% packet loss, 5% corrupted frames and random delays. |
| **Scale** | `config/scenarios/many_sensors.json` | 200 sensors on 200 connections, about 10,000 messages/s. |
| **Overload** | `many_sensors.json` + `config/scenarios/overload_server.json` | An under-provisioned server: the queue fills up and the drop policy takes over while the server stays responsive. |

---

## 12. How to run it

Requirements: Linux (Ubuntu 22.04/24.04, or WSL2 on Windows), a C++17 compiler, CMake ≥ 3.22,
Ninja.

```bash
# build and test
sudo apt-get install -y build-essential cmake ninja-build libgtest-dev nlohmann-json3-dev
cmake --preset release
cmake --build --preset release
ctest --preset release

# run (two terminals)
./build/release/apps/telemetry_server --config config/server.json
./build/release/apps/sensor_sim --config config/simulator.json

# or the scripted 40-second fault demo
scripts/demo.sh

# or with Docker
docker compose up --build
SIM_CONFIG=scenarios/faults.json docker compose up
```

---

## 13. The interactive visualizer

`visualizer/index.html` is a single web page that shows the system working, message by
message, in any browser ([live version](https://shivang1141.github.io/Real-Time-Sensor-Telemetry-and-Control-Simulator/)).

* Dots travel from each sensor through its connection thread, the decoder, the router, a
  queue and a worker. Broken messages are visibly rejected, lost ones fade out, and full
  queues visibly drop messages.
* **Trace a message** pauses at each of six stages and explains what the server does there,
  with the real message, queue number and detector checks.
* **Inject fault** lets you break any sensor live and watch the server react.
* Live metrics, a latency histogram, per-sensor charts with alarm limits, an event log, and a
  status console in the same format as the real server.

It is a JavaScript model of the server's logic (same thresholds, rules, sharding, batching and
drop policy), not the C++ program itself. It is slowed down so each message can be followed.

---

## 14. Scope and honest limitations

* **Educational project, synthetic data.** It is not flight, medical or defense software,
  and is not certified for any safety-critical use.
* **Not hard real-time.** Latency is measured, not guaranteed. There is no real-time
  scheduling, CPU pinning or memory locking.
* **Thread per connection** is simple and fine for hundreds of clients, but would not scale
  to tens of thousands; an `epoll` event loop would be the next step.
* **JSON parsing** is the main CPU cost at high rates.
* **No authentication or encryption.** Run it on trusted networks only.
* **In-memory state only.** There is no database, persistence or replay.
* **Benchmarks** were run on a laptop under WSL2 with the load generator on the same machine,
  so the absolute numbers are specific to that machine.
* The "control" half of the name (commands from server to sensor) is planned, not built.

---

## 15. What could come next

* Faster, schema-specific JSON parsing (removes about 25–30% of CPU).
* Batching on the producer side to reduce thread wake-ups.
* An `epoll`-based network layer and a binary protocol for very high-rate sensors.
* A Prometheus-style metrics endpoint and a live web dashboard connected to the real server.
* A command channel from server to sensors, adding the "control" side.

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **Telemetry** | Measurements sent automatically from a device to a central system. |
| **TCP** | The standard reliable network connection. It delivers a stream of bytes, not separate messages. |
| **NDJSON** | Newline-delimited JSON: one JSON object per line. |
| **Framing** | Splitting a byte stream back into individual messages, here at each newline. |
| **Thread** | An independent sequence of work running concurrently inside a program. |
| **Producer–consumer** | One set of threads puts work into a queue; another set takes it out and does it. |
| **Bounded queue** | A queue with a fixed maximum size, so memory cannot grow without limit. |
| **Shard** | One of several independent partitions; here a queue plus its worker plus the sensors assigned to it. |
| **Hash routing** | Using a hash of the sensor id to pick its shard, so each sensor always maps to the same one. |
| **Backpressure / drop policy** | What happens when work arrives faster than it can be done. This server drops the newest message and counts it. |
| **Hysteresis** | Different thresholds for raising and clearing an alarm, so it doesn't flicker. |
| **Edge-triggered** | Reporting only when a condition starts or stops, not on every message. |
| **Stale** | No fresh data from a sensor for longer than a timeout. |
| **Sequence number** | A counter in each message; a jump reveals lost messages. |
| **Latency, p50 / p95 / p99** | Time to process a message. p99 means 99% of messages were faster than this value. |
| **Throughput** | Messages processed per second. |
| **Wait-free / relaxed atomics** | Counters that many threads can update at once without locks or waiting. |
| **Sanitizer** | A compiler feature that instruments the program to catch memory errors (ASan), undefined behavior (UBSan) or data races (TSan) while tests run. |
| **RAII** | A C++ pattern where an object releases its resource (such as a socket) automatically when it goes out of scope. |
| **eventfd** | A Linux object used as a lightweight signal between threads. |
| **CI** | Continuous integration: every change is built and tested automatically. |
