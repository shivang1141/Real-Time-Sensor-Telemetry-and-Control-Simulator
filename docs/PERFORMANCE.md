# Performance

All numbers on this page were measured. They come from runs of
`scripts/run_benchmarks.sh` and `bench_components` whose raw output is committed in
`benchmarks/results/`.

## Environment

| | |
|---|---|
| CPU | Intel Core i5-1245U (laptop; 2 P-cores + 8 E-cores, 12 logical CPUs) |
| RAM | 7.6 GiB available to WSL2 |
| OS | Ubuntu 24.04 on WSL2, kernel 6.6.87.2-microsoft-standard-WSL2 |
| Compiler | GCC 13.3, `-O3` (Release preset) |

**Caveats:** The load generator and the server run on the **same machine** and compete
for the same 12 logical CPUs. WSL2 adds virtualization overhead to syscalls and
scheduling. Laptop frequency scaling causes run-to-run variance, so every case is run 3
times and the median reported, with the min–max range shown. Treat the absolute numbers
as specific to this machine; the *relationships* between configurations are the point.

## Methodology

* `telemetry_server` and `telemetry_loadgen` run as **separate processes**, connected over
  loopback TCP.
* The load generator uses 8 connections × 10 sensors and batches 32 frames per write
  (1 frame per write in the `_b1` case). Its traffic is healthy (in range, no alarms), so
  the runs measure the pipeline rather than alarm handling.
* 10 s per run, 3 runs per case. Server CPU comes from `/proc/<pid>/stat`
  (utime + stime; 100% = one core), and peak RSS from `VmHWM`.
* Throughput = messages processed ÷ load duration. Latency = frame received → processing
  finished, on the server's steady clock (see `METRICS.md`).
* `simulated_work_us` busy-waits per message to model heavier processing algorithms. The
  real detection logic costs about 0.2 µs per message, so without it the workers are almost
  never the bottleneck.

Reproduce with:

```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/bench_components
scripts/run_benchmarks.sh            # ~6 min; DURATION / REPEATS / CASES env vars
```

## Per-message cost of each stage (`bench_components`)

| Stage | ns / message |
|---|---:|
| `protocol::decode` (nlohmann::json DOM) | 2,528 |
| `protocol::encode` | 2,983 |
| `SensorRegistry::update` (validation + detection + state) | 180 |
| `LineFramer` (16 KiB chunks) | 15 |
| `LatencyHistogram::record` (1 thread) | 28 |
| `BoundedQueue` push+pop, 1 producer / 1 consumer | 374 |

**JSON decoding costs about 14× more than the anomaly detection itself.**

## Pipeline results (median of 3 × 10 s)

| Case | Workers | Work/msg | Offered | Throughput (min–max) | Dropped | p50 | p95 | p99 | Server CPU | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| light_w1 | 1 | 0 µs | max | 339,969/s (286,992–341,519) | 6.5% | 2.9 ms | 18 ms | 30 ms | 671% | 4.6 MB |
| light_w2 | 2 | 0 µs | max | 381,141/s (308,946–415,128) | 0.0% | 2.4 ms | 11 ms | 19 ms | 739% | 4.5 MB |
| light_w4 | 4 | 0 µs | max | 311,397/s (282,107–343,802) | 0.0% | 2.6 ms | 13 ms | 20 ms | 804% | 4.4 MB |
| cpu20_w1 | 1 | 20 µs | max | 24,887/s (24,024–26,412) | 94.4% | 160 ms | 270 ms | 328 ms | 800% | 4.8 MB |
| cpu20_w2 | 2 | 20 µs | max | 47,928/s (39,992–49,226) | 89.0% | 168 ms | 295 ms | 311 ms | 890% | 5.1 MB |
| cpu20_w4 | 4 | 20 µs | max | 74,382/s (68,014–109,562) | 76.9% | 217 ms | 328 ms | 360 ms | 1007% | 6.1 MB |
| cpu20_w8 | 8 | 20 µs | max | 124,117/s (122,008–172,145) | 45.2% | 242 ms | 377 ms | 459 ms | 1084% | 8.2 MB |
| steady20k_w2 | 2 | 0 µs | 20,000/s | 20,013/s | 0.0% | 287 µs | 735 µs | 1.1 ms | 44% | 4.1 MB |
| steady10k_w2_b1 | 2 | 0 µs | 10,000/s | 9,998/s | 0.0% | 115 µs | 391 µs | 3.6 ms | 179% | 4.0 MB |
| overload_w1 | 1 | 50 µs | 40,000/s | 17,899/s (17,576–18,668) | 55.2% | 221 ms | 311 ms | 426 ms | 143% | 4.2 MB |

### What the results show

* **Normal load is handled with sub-millisecond latency.** At 20k msg/s every message is
  processed, with p50 287 µs and p99 1.1 ms. With unbatched sends (`_b1`) the median is
  115 µs. In the batched case, much of the median comes from the 32 frames in a write
  being decoded one after another. The `_b1` p99 of 3.6 ms comes from OS scheduling
  under WSL2: one wake-up per message, and no real-time priority.
* **Worker scaling when processing dominates (`cpu20_*`).** Going from 1 to 8 workers
  raises throughput 5.0× (24.9k → 124k msg/s). The ideal single-worker rate at 20 µs/msg
  is about 47k/s. The shortfall, and the sub-linear scaling, come from CPU
  oversubscription: the load generator's 8 threads and the server's 8 connection threads
  run flat out on the same 12 logical CPUs, 8 of which are E-cores.
* **Without artificial work, adding workers does not help (`light_*`).** Throughput
  plateaus at about 310–380k msg/s with 1, 2 or 4 workers. Workers are not the
  bottleneck here; decoding on the connection threads and the co-located load generator
  are. light_w4 being lower than light_w2 is within the run-to-run spread and reflects
  extra threads competing for cores.
* **Overload is handled as designed (`overload_w1`).** The server is offered 40k msg/s
  against a capacity of about 20k msg/s. It processes 17.9k/s (90% of theoretical), drops
  the rest (55%) and counts every drop, while memory stays flat at 4.2 MB. Latency is
  bounded by queue capacity × service time (4096 × 50 µs ≈ 205 ms; p50 measured 221 ms).
  Shrinking `queue_capacity_per_worker` trades more drops for lower latency.
* **Memory is small and bounded.** Peak RSS is 4–9 MB in every case, including floods of
  millions of messages, because queues and per-connection buffers are fixed-size.

## Optimization: consumer starvation in the bounded queue

The first benchmark pass (5 s runs, `benchmarks/results/before_optimization_5s.csv`)
showed workers falling far behind what the component costs predicted. One worker with no
artificial work handled only 107k msg/s, about 9 µs per message, against roughly 1 µs of
real work. In the controlled overload case (40k offered), however, the worker reached its
theoretical rate. The slowdown only appeared when producers **hammered a full queue**: 8
connection threads calling `try_push` about 500k times per second, each attempt taking the
mutex, starved the single consumer of the lock.

Two changes, both in `BoundedQueue`:

1. **Lock-free fast rejection.** An atomic copy of the size, updated under the lock, lets
   `try_push` drop without touching the mutex when the queue is full. Capacity is still
   enforced exactly under the lock.
2. **Batch dequeue.** Workers take up to 64 items per lock acquisition (`pop_batch`).

Same harness, same load generator, 5 s runs, before → after:

| Case | Before | After | Change |
|---|---:|---:|---:|
| light_w1 | 107,486/s, 78.5% dropped | 352,832/s, 9.9% dropped | 3.3× |
| light_w2 | 277,041/s, 52.7% dropped | 492,053/s, 0.6% dropped | 1.8× |
| light_w4 | 297,117/s, 17.3% dropped | 536,380/s, 0.1% dropped | 1.8× |
| cpu20_w1 | 18,002/s | 36,216/s | 2.0× |
| cpu20_w8 | 83,196/s | 168,542/s | 2.0× |
| light_w2 p50 latency | 29.7 ms | 1.8 ms | −94% |

Both of those 5 s runs used an early version of the load generator, whose varying values
also triggered alarm logging. The comparison is still like-for-like, because only the
queue changed between them, but the absolute numbers are not comparable to the final
table above (different load-generator traffic, single runs, 10 s vs 5 s).

## Profile of the remaining costs (`perf`, light_w2)

`perf record -g` on the server after the optimization:

| Share of server CPU | Where |
|---|---|
| ~25–30% | nlohmann::json DOM parsing: lexer, parser, `malloc`/`free`, string building, DOM destruction |
| ~11% | Kernel `try_to_wake_up`, mostly from `submit` waking an idle worker (futex) |
| ~3% | Futex waits on shard mutex contention |
| ~3% | `LatencyHistogram::record` (shared atomic cache lines across workers) |
| ~2.5% | `clock_gettime` (vDSO) |

Profiling also caught a **benchmark bug** first. The original load generator sent varying
values microseconds apart, which correctly triggered `rate_of_change` alarms, about 167k
log lines in 6 s, and log I/O dominated the profile. Two fixes followed: the load
generator now sends healthy, constant values, and log output for condition transitions is
now rate-limited (200 lines/s, with every event still counted), so a flapping sensor cannot
make the server spend its CPU on logging.

### Not yet optimized, and why

The server already sustains over 300k msg/s while sharing a laptop with its load generator,
far beyond what the simulated fleets need (the 200-sensor scenario offers about 10k
msg/s). The next steps, measured but deliberately not implemented, are:

1. **SAX or schema-specific parsing** to avoid DOM allocation: expected to remove most of
   the ~25–30%.
2. **Producer-side batching**: one `try_push` batch per `recv` chunk instead of per frame,
   which removes most worker wake-ups and mutex traffic.
3. **Per-worker histograms** merged at snapshot time, to remove shared-cache-line traffic.
