# Testing

## Running the tests

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

| Preset | What it is for |
|---|---|
| `debug` | Day-to-day development. `-Werror` with a strict warning set (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast ...`). |
| `release` | Optimized build; also used for benchmarks. Tests run here too, because timing-sensitive bugs often only show up in fast builds. |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer: out-of-bounds access, use-after-free, leaks, signed overflow, misaligned access. Any finding aborts the test. |
| `tsan` | ThreadSanitizer: data races and lock-order inversions. Use a separate build directory from `asan`; the two cannot be combined. |

Useful filters:

```bash
ctest --preset debug -L unit                 # fast, no sockets or threads beyond the queue tests
ctest --preset debug -L integration          # real TCP on 127.0.0.1, real threads
ctest --preset debug -R BoundedQueue         # by name
./build/debug/tests/unit_tests --gtest_filter='AnomalyDetector.*'
ctest --preset debug -L integration --repeat until-fail:20   # flakiness hunt
```

If ThreadSanitizer aborts at start-up with `unexpected memory mapping`, the kernel's mmap
randomization is too high for the TSan runtime: `sudo sysctl vm.mmap_rnd_bits=28` (CI does
this). It is not a bug in the project.

## What is covered

### Unit tests (`tests/unit`, 92 tests)

| Suite | Covers |
|---|---|
| `Protocol` | Round trips for every sensor type; each class of malformed input (structure, missing fields, wrong types, bad ID, unknown enum, timestamp range, overflow). |
| `LineFramer` | Several frames per chunk, frames split across chunks, byte-at-a-time delivery, `\r\n`, empty lines, frame exactly at the limit, oversized frames inside and across chunks, resynchronization, bounded buffering. |
| `BoundedQueue` | FIFO, capacity, untouched item on rejected push, close-then-drain, close wakes blocked producers/consumers, blocking push, `pop_batch`, size-hint refresh, concurrent `try_push` never exceeding capacity, 4×4 producer/consumer stress where every item must arrive exactly once. |
| `AnomalyDetector` | Every limit and its boundary, hysteresis on both sides, status conditions, rate of change (noise rejection, real jumps, short history), trend minimum samples, least-squares slope accuracy at realistic epoch timestamps, every validation rule. |
| `SensorRegistry` | Registration, latest value, edge-triggered events, health priority, sequence gaps, rejection without state change, restart detection, stale raise/clear, rejected data not refreshing freshness. Time is controlled with `ManualClock`, so no test sleeps. |
| `LatencyHistogram` | Exact small values, bucket bounds and error ≤ 1/32 across the range, percentile accuracy against an exact sort of 100k log-normal samples, interval subtraction, 8-thread concurrent recording. |
| `StatusReporter` | Interval rate math, only unhealthy sensors listed, JSON export parses and is complete. |
| `ServerConfig` / `SimulatorConfig` | Defaults, overrides, partial thresholds, unknown-field rejection with precise paths, type and range errors, every shipped config file loads. |
| `SensorModel` | Determinism per seed, noise statistics, ramp, failure windows, fault-status mode, drop/malformed probabilities, corrupted frames never decode, delay range. |
| `Logger` / `RateLimiter` | logfmt format, quoting/escaping, level filtering, rate-limit windows and suppressed counts. |

### Integration tests (`tests/integration`, 22 tests)

| Test | Scenario |
|---|---|
| `TcpServerTest.DeliversFramesFromOneClient` | Sensor connects and sends. |
| `TcpServerTest.ReassemblesFramesSentByteByByte` | Partial TCP reads, one byte per segment. |
| `TcpServerTest.ManyConcurrentClientsKeepPerConnectionOrder` | 16 clients × 300 frames concurrently; per-connection order preserved. |
| `TcpServerTest.OversizedFrameIsDiscardedAndConnectionSurvives` | 10 KB line against a 64-byte limit. |
| `TcpServerTest.DetectsClientDisconnect` | Client closes mid-frame; partial frame not delivered. |
| `TcpServerTest.IdleTimeoutClosesConnection` | Silent client is disconnected. |
| `TcpServerTest.RejectsConnectionsBeyondLimit` | Connection cap enforced, capacity freed on disconnect. |
| `TcpServerTest.StopClosesActiveConnectionsPromptly` | Shutdown with connected clients completes in < 2 s. |
| `TcpServerTest.StartFailsWhenPortInUse` | Bind failure surfaces as an exception. |
| `TelemetryServiceTest.SingleSensorEndToEnd` | Frame → decode → queue → worker → registry. |
| `TelemetryServiceTest.ManySensorsSendConcurrently` | 20 sensors × 250 messages, 4 workers: no drops, no gaps, per-sensor order kept. |
| `TelemetryServiceTest.PartialTcpWritesAreReassembled` | Random chunk sizes splitting frames. |
| `TelemetryServiceTest.InvalidAndOversizedMessagesAreCountedAndConnectionSurvives` | Invalid input is counted and the connection keeps working. |
| `TelemetryServiceTest.SemanticallyInvalidMessagesAreRejected` | Implausible value, duplicate sequence. |
| `TelemetryServiceTest.ClientDisconnectMidFrameIsHandled` | Server keeps serving new clients. |
| `TelemetryServiceTest.QueueSaturationDropsNewestAndAccountsForEveryFrame` | 1 slow worker, 8-slot queue: drops occur, the queue stays bounded, every frame is accounted for, and every drop shows up downstream as a sequence gap. |
| `TelemetryServiceTest.GracefulShutdownDrainsQueuedMessages` | `stop()` processes everything already queued. |
| `TelemetryServiceTest.ShutdownWithIdleConnectedClientsIsPrompt` | No hang on idle sockets. |
| `TelemetryServiceTest.DetectsAnomaliesAndStaleSensors` | Critical value and stale detection through the full stack. |
| `SimulatorTest.StreamsTelemetryToServer` | Real simulator → real server; sent == received. |
| `SimulatorTest.DisconnectFailureReconnectsAndServerSeesGapFree` | Disconnect fault, reconnect, no gaps. |
| `SimulatorTest.ReconnectsWhenServerStartsLate` | Backoff and reconnect when the server is not up yet. |

## Test design principles

* **No fixed sleeps for synchronization.** Tests poll a condition with a deadline
  (`test::wait_until`), so they finish as soon as the condition holds and tolerate slow
  sanitizer builds.
* **Ephemeral ports.** Servers bind port 0, so tests run in parallel (`ctest -j`) without
  conflicts.
* **Injected time.** Staleness and timestamp logic use `ManualClock`.
* **Flakiness checks.** The integration suite was repeated 20× (release, debug) and 8×
  (ASan, TSan) with `-j12` on a 12-thread machine with no failures. Getting there exposed
  two timing assumptions in the tests themselves, both fixed. The saturation test assumed
  drops would be followed by accepted messages, which is not true when a fast build
  finishes the whole burst while the queue is still full. The shutdown-drain test assumed
  sending would always finish before the worker caught up, which failed under a heavily
  loaded ASan run.

## Interpreting failures

* **An `EXPECT_*` with a counter mismatch** in an integration test: compare the
  invariants in `METRICS.md`. For example, `frames_received != invalid + dropped +
  enqueued` points at accounting in `TelemetryService::on_frame` or `ProcessingEngine::submit`.
* **A `wait_until` returned false**: the condition never became true within its deadline
  (usually 5–10 s). Either something hung (rerun with `--gtest_filter` and attach `gdb`)
  or the machine is extremely overloaded.
* **ASan/UBSan report**: the first stack frame inside `src/` or `include/` is the culprit.
  Look at the "freed by" / "previously allocated by" stacks for use-after-free.
* **TSan `data race` report**: two stacks touching the same address, at least one a write,
  without a common lock. Check the synchronization inventory in `ARCHITECTURE.md` to see
  which lock should protect that data.
