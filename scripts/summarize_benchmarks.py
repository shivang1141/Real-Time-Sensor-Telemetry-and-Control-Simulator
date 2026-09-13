#!/usr/bin/env python3
"""Turns benchmarks/results/results.csv into a Markdown summary (median of runs per case).

Usage: summarize_benchmarks.py results.csv [machine.json] > summary.md
"""
import csv
import json
import statistics
import sys
from collections import OrderedDict


def main() -> None:
    rows = list(csv.DictReader(open(sys.argv[1])))
    machine = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else None

    cases: "OrderedDict[str, list]" = OrderedDict()
    for row in rows:
        cases.setdefault(row["case"], []).append(row)

    def med(runs, key):
        return statistics.median(float(r[key]) for r in runs)

    def spread(runs, key):
        values = [float(r[key]) for r in runs]
        return min(values), max(values)

    if machine:
        print(f"Machine: {machine['cpu']}, {machine['logical_cpus']} logical CPUs, "
              f"{machine['mem_gib']} GiB RAM, kernel {machine['kernel']}, {machine['compiler']}.")
        print()
    runs = len(next(iter(cases.values())))
    print(f"Median of {runs} runs per case. Throughput = messages processed per second. "
          "Latency = frame received -> processing finished (server-side, steady clock).")
    print()
    print("| Case | Workers | Work/msg | Offered | Throughput (min-max) | Dropped | "
          "p50 | p95 | p99 | Server CPU | Peak RSS |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for name, runs in cases.items():
        first = runs[0]
        offered = "max" if first["target_rate"] == "0" else f"{int(float(first['target_rate'])):,}/s"
        lo, hi = spread(runs, "throughput")
        print(f"| {name} | {first['workers']} | {first['simulated_work_us']} us | {offered} | "
              f"{med(runs, 'throughput'):,.0f}/s ({lo:,.0f}-{hi:,.0f}) | "
              f"{med(runs, 'drop_pct'):.1f}% | "
              f"{fmt_us(med(runs, 'p50_us'))} | {fmt_us(med(runs, 'p95_us'))} | "
              f"{fmt_us(med(runs, 'p99_us'))} | {med(runs, 'server_cpu_pct'):.0f}% | "
              f"{med(runs, 'server_peak_rss_mb'):.1f} MB |")


def fmt_us(value: float) -> str:
    if value >= 10_000:
        return f"{value / 1000:.0f} ms"
    if value >= 1000:
        return f"{value / 1000:.1f} ms"
    return f"{value:.0f} us"


if __name__ == "__main__":
    main()
