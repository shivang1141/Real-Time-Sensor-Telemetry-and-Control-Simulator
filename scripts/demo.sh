#!/usr/bin/env bash
# Repeatable demonstration: starts the server, runs the fault-injection scenario against
# it, then shuts the server down gracefully and summarizes what was detected.
#
# Usage: scripts/demo.sh [build_dir] [scenario]
#   build_dir  default: build/release (built automatically if missing)
#   scenario   default: config/scenarios/faults.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build/release}"
SCENARIO="${2:-$ROOT/config/scenarios/faults.json}"
PORT="${PORT:-9000}"
OUT="$(mktemp -d)"

if [[ ! -x "$BUILD/apps/telemetry_server" ]]; then
    echo "building release preset..."
    (cd "$ROOT" && cmake --preset release > /dev/null && cmake --build --preset release)
fi

echo "logs: $OUT/server.log"
"$BUILD/apps/telemetry_server" --config "$ROOT/config/server.json" --port "$PORT" \
    --status-interval-ms 5000 --log-file "$OUT/server.log" &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null || true' EXIT
sleep 0.5

"$BUILD/apps/sensor_sim" --config "$SCENARIO" --port "$PORT" --log-level warn

echo
echo "stopping server (SIGINT -> graceful shutdown)..."
kill -INT "$SERVER_PID"
wait "$SERVER_PID"
trap - EXIT

echo
echo "==== events detected (from structured log) ===="
grep -oE 'event=(condition_raised|condition_cleared|sequence_gap|measurement_rejected|invalid_message|sensor_registered|client_disconnected)' \
    "$OUT/server.log" | sort | uniq -c | sort -rn
echo
echo "==== conditions raised per sensor ===="
grep 'event=condition_raised' "$OUT/server.log" \
    | sed -E 's/.*sensor=([^ ]+) condition=([^ ]+).*/\1 \2/' | sort | uniq -c
