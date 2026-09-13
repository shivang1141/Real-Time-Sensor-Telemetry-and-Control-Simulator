#!/usr/bin/env bash
# Pipeline benchmarks: runs telemetry_server and telemetry_loadgen as separate processes
# for each configuration and records throughput, latency, drops, CPU and memory.
#
# Usage: scripts/run_benchmarks.sh [build_dir] [output_dir]
#   DURATION=10   seconds per run
#   REPEATS=3     runs per case (summarize_benchmarks.py reports the median)
#   CASES="..."   space-separated subset of case names (default: all)
#
# Output: <output_dir>/results.csv plus raw per-run JSON/logs in <output_dir>/raw/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build/release}"
OUT="${2:-$ROOT/benchmarks/results}"
DURATION="${DURATION:-10}"
REPEATS="${REPEATS:-3}"
SERVER="$BUILD/apps/telemetry_server"
LOADGEN="$BUILD/benchmarks/telemetry_loadgen"
RAW="$OUT/raw"
CLK_TCK="$(getconf CLK_TCK)"

for bin in "$SERVER" "$LOADGEN"; do
    [[ -x "$bin" ]] || { echo "missing $bin (build the release preset first)" >&2; exit 1; }
done
mkdir -p "$RAW"

# name workers capacity_per_worker simulated_work_us connections sensors_per_conn rate(0=max) batch
#   light_*      no artificial work, unthrottled: maximum pipeline throughput
#   cpu20_*      20 us of CPU work per message: worker scaling when processing dominates
#   steady*      fixed offered load below capacity: latency under normal operation
#   overload_w1  offered 40k msg/s against ~20k msg/s capacity: drop policy under overload
ALL_CASES=(
    "light_w1          1 4096  0  8 10     0 32"
    "light_w2          2 4096  0  8 10     0 32"
    "light_w4          4 4096  0  8 10     0 32"
    "cpu20_w1          1 4096 20  8 10     0 32"
    "cpu20_w2          2 4096 20  8 10     0 32"
    "cpu20_w4          4 4096 20  8 10     0 32"
    "cpu20_w8          8 4096 20  8 10     0 32"
    "steady20k_w2      2 4096  0  8 10 20000 32"
    "steady10k_w2_b1   2 4096  0  8 10 10000  1"
    "overload_w1       1 4096 50  8 10 40000 32"
)

cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat"; }

wait_for_port() {
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then return 0; fi
        sleep 0.05
    done
    echo "server did not open port $1" >&2
    return 1
}

machine_info() {
    python3 - <<'EOF'
import json, os, platform, subprocess
cpu = "unknown"
try:
    for line in open("/proc/cpuinfo"):
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip(); break
except OSError:
    pass
mem_kb = 0
for line in open("/proc/meminfo"):
    if line.startswith("MemTotal"):
        mem_kb = int(line.split()[1]); break
print(json.dumps({"cpu": cpu, "logical_cpus": os.cpu_count(), "mem_gib": round(mem_kb / 1048576, 1),
                  "kernel": platform.release(),
                  "compiler": subprocess.run(["c++", "--version"], capture_output=True, text=True).stdout.splitlines()[0]}))
EOF
}

machine_info > "$OUT/machine.json"
echo "machine: $(cat "$OUT/machine.json")"
echo "duration per run: ${DURATION}s, runs per case: ${REPEATS}"
echo "case,run,workers,capacity_per_worker,simulated_work_us,connections,sensors,target_rate,batch,sent,send_rate,frames_received,processed,throughput,dropped,drop_pct,invalid,p50_us,p95_us,p99_us,max_us,queue_wait_p99_us,peak_shard_queue,server_cpu_pct,server_peak_rss_mb" > "$OUT/results.csv"

port_base=$((20000 + RANDOM % 20000))
i=0
for spec in "${ALL_CASES[@]}"; do
    read -r name workers capacity work conns sensors rate batch <<< "$spec"
    if [[ -n "${CASES:-}" && " $CASES " != *" $name "* ]]; then continue; fi
    for run in $(seq 1 "$REPEATS"); do
        port=$((port_base + i)); i=$((i + 1))
        tag="$name.run$run"

        "$SERVER" --port "$port" --workers "$workers" --queue-capacity "$capacity" \
            --simulated-work-us "$work" --log-level warn --log-file "$RAW/$tag.server.log" \
            --quiet --metrics-json "$RAW/$tag.server.json" > /dev/null &
        pid=$!
        wait_for_port "$port"
        cpu_before=$(cpu_ticks "$pid")

        "$LOADGEN" --port "$port" --connections "$conns" --sensors-per-connection "$sensors" \
            --rate "$rate" --batch "$batch" --duration "$DURATION" > "$RAW/$tag.loadgen.json"

        cpu_after=$(cpu_ticks "$pid")
        rss_kb=$(awk '/VmHWM/ {print $2}' "/proc/$pid/status")
        kill -INT "$pid"
        wait "$pid"

        python3 - "$name" "$run" "$tag" "$workers" "$capacity" "$work" "$batch" "$RAW" \
            "$cpu_before" "$cpu_after" "$CLK_TCK" "$rss_kb" >> "$OUT/results.csv" <<'EOF'
import json, sys
name, run, tag, workers, capacity, work, batch, raw, cb, ca, tck, rss = sys.argv[1:]
lg = json.load(open(f"{raw}/{tag}.loadgen.json"))
sv = json.load(open(f"{raw}/{tag}.server.json"))
m, lat, qw = sv["messages"], sv["processing_latency"], sv["queue_wait"]
dur = lg["duration_s"]
cpu_pct = (int(ca) - int(cb)) / int(tck) / dur * 100
drop_pct = 100.0 * m["dropped"] / m["frames_received"] if m["frames_received"] else 0.0
print(",".join(str(x) for x in [
    name, run, workers, capacity, work, lg["connections"], lg["sensors"], lg["target_rate"],
    batch, lg["sent"], round(lg["send_rate"]), m["frames_received"], m["processed"],
    round(m["processed"] / dur), m["dropped"], round(drop_pct, 2), m["invalid"],
    lat["p50_us"], lat["p95_us"], lat["p99_us"], lat["max_us"], qw["p99_us"],
    sv["queue"]["peak_shard_depth"], round(cpu_pct), round(int(rss) / 1024, 1)]))
EOF
        tail -1 "$OUT/results.csv"
    done
done
python3 "$ROOT/scripts/summarize_benchmarks.py" "$OUT/results.csv" "$OUT/machine.json" > "$OUT/summary.md"
echo "summary written to $OUT/summary.md"

echo "results written to $OUT/results.csv"
