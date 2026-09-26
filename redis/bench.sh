#!/usr/bin/env bash
# Measures command throughput with redis-benchmark (network + protocol included).
set -euo pipefail
cd "$(dirname "$0")"

PORT=${REDIS_BENCH_PORT:-6398}
N=${BENCH_N:-200000}
IDGENKIT_RELID_SECRET=bench-only-secret-0123456789 redis-server --port "$PORT" --save "" --appendonly no \
  --daemonize no --loadmodule "$PWD/build/idgenkit.so" >build/redis-bench.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null || true' EXIT
for _ in $(seq 50); do redis-cli -p "$PORT" ping >/dev/null 2>&1 && break; sleep 0.1; done

echo "# Redis $(redis-server --version | awk '{sub(/^v=/, "", $3); print $3}'), N=$N, 50 clients, pipeline 16"
for cmd in "PING" "ULID.GENERATE" "ULID.MONOTONIC" "UUIDV4.GENERATE" "UUIDV7.GENERATE" "UUIDV7.MONOTONIC" "RELID.GENERATE customer-42" "RELID.MONOTONIC customer-42" "SNOWFLAKE.GENERATE" "NANOID.GENERATE" "NANOID.GENERATE 21 0123456789abcdef"; do
  rps=$(redis-benchmark -p "$PORT" -n "$N" -c 50 -P 16 -q $cmd | awk -F'[ :]+' '/requests per second/ {for (i=1;i<=NF;i++) if ($i=="requests") print $(i-1)}' | tail -1)
  printf "redis   %-36s %14s req/s\n" "$cmd" "$rps"
done
