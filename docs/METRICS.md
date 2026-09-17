# Metrics reference

All counters are monotonic since server start, held in `Metrics` (`metrics/metrics.hpp`)
and updated with relaxed atomics. They appear in the periodic status block, the final
summary, and the `--metrics-json` file.

## Counters

| Metric | Incremented when | Thread |
|---|---|---|
| `connections_accepted` | a connection thread starts | connection |
| `connections_rejected` | a client is turned away because `max_connections` is reached | acceptor |
| `connections_closed` | a connection thread exits, for any reason | connection |
| `frames_received` (`recv`) | the framer yields a complete, non-empty line within the size limit | connection |
| `frames_oversized` | a line exceeds `max_frame_bytes` (not included in `frames_received`) | connection |
| `messages_invalid` | a received frame fails `protocol::decode` | connection |
| `messages_enqueued` | `try_push` succeeds | connection |
| `messages_dropped` | the target shard queue is full at submit time | connection |
| `messages_processed` | a worker finishes handling a dequeued message (accepted or rejected) | worker |
| `messages_rejected` | a processed message fails semantic validation | worker |
| `sequence_missing` (`seq_missing`) | sum of `seq - last_seq - 1` over detected gaps | worker |

### Invariants

Once traffic has stopped (and exactly after a clean shutdown):

```
frames_received    == messages_invalid + messages_dropped + messages_enqueued
messages_processed == messages_enqueued
```

While running, `messages_enqueued - messages_processed` is the number of messages in
queues or in progress. Because counters are read individually (not as an atomic cut), a
live snapshot can be off by the handful of messages in flight. The invariants are asserted
in `QueueSaturationDropsNewestAndAccountsForEveryFrame`.

## Gauges (sampled at report time)

| Gauge | Meaning |
|---|---|
| `active connections` | connection threads currently running |
| `queue depth` | sum of items currently in all shard queues |
| `queue capacity` | `workers × queue_capacity_per_worker` |
| `peak_shard` | the largest depth any single shard reached since start (bounded by the per-shard capacity) |
| sensor health counts | sensors per health state, derived from active conditions |
| active warnings / critical | number of active conditions of each severity, summed over sensors |

## Rates

`recv/s` and `processed/s` in the status block are deltas between two consecutive status
samples divided by the steady-clock time between them, so they describe the last interval
(default 1 s), not the average since start.

## Latency

All latencies are recorded in microseconds into `LatencyHistogram`: log-linear buckets
(exact below 64 µs, then 32 sub-buckets per power of two). A reported percentile is the
upper bound of the bucket containing the `ceil(q·n)`-th smallest sample, so it **never
under-reports** and over-reports by at most ~3.1%.

| Histogram | Start | End | Clock |
|---|---|---|---|
| `processing_latency` | frame's last byte read from the socket | worker finished processing | steady (server) |
| `queue_wait` | message pushed into the shard queue | worker dequeued it | steady (server) |
| `end_to_end_latency` | sender's `ts` field | worker finished processing | wall clocks of two hosts |

* `processing_latency` includes decode time, queue wait and processing. It is the
  server's own contribution and needs no clock synchronization.
* `end_to_end_latency` additionally includes the sender's scheduling and the network. It
  is only valid if both clocks agree (same host, or NTP/PTP). Negative values, caused by
  clock skew, are recorded as 0. It is exported in `--metrics-json` but deliberately not
  shown as the headline number.
* The status block shows **interval** percentiles: the histogram snapshot minus the
  previous one, i.e. only samples recorded in the last interval. The final summary and
  JSON show cumulative percentiles since start.

## Throughput in benchmarks

`throughput = messages_processed / load duration`. The duration is measured by the load
generator. Messages drained from queues after the load stops are included, which can
overstate throughput by at most one queue capacity (4096 per shard) over a 10 s run.
