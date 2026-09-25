#!/usr/bin/env bash
# Builds the extension in Docker, then runs pg_regress, a multi-session
# uniqueness check, and (with --bench) SQL benchmarks.
#   PG_MAJOR=17 ./postgres/test.sh [--bench]
set -euo pipefail
cd "$(dirname "$0")/.."

PG_MAJOR=${PG_MAJOR:-17}
IMAGE="idgenkit-pg:$PG_MAJOR"
NAME="idgenkit-pg-test-$PG_MAJOR"
docker build -q -f postgres/Dockerfile --build-arg PG_MAJOR="$PG_MAJOR" -t "$IMAGE" . >/dev/null

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" -e POSTGRES_HOST_AUTH_METHOD=trust "$IMAGE" \
  -c shared_preload_libraries=idgenkit -c idgenkit.machine_id=7 \
  -c idgenkit.snowflake_epoch_ms=1288834974657 >/dev/null
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT

psql() { docker exec -i "$NAME" psql -U postgres -X -q -A -t -v ON_ERROR_STOP=1 "$@"; }
for _ in $(seq 60); do psql -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done
# The image's init runs a temporary server first; wait for the final one.
sleep 1
for _ in $(seq 60); do psql -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done

echo "== pg_regress (PostgreSQL $PG_MAJOR)"
if ! docker exec -u postgres "$NAME" make -C /src/postgres installcheck PGUSER=postgres; then
  docker exec "$NAME" cat /src/postgres/test/regression.diffs || true
  exit 1
fi

echo "== snowflake uniqueness across 8 concurrent sessions"
psql -d postgres -c "CREATE EXTENSION IF NOT EXISTS idgenkit" -c "CREATE TABLE sf (id bigint PRIMARY KEY)"
pids=()
for _ in $(seq 8); do
  psql -d postgres -c "INSERT INTO sf SELECT snowflake_generate() FROM generate_series(1, 50000)" & pids+=($!)
done
wait "${pids[@]}"
count=$(psql -d postgres -c "SELECT count(*) FROM sf")
[[ "$count" == "400000" ]] && echo "PASS 400000 unique ids" || { echo "FAIL got $count"; exit 1; }

echo "== without shared_preload_libraries"
NOPRE="$NAME-nopreload"
docker rm -f "$NOPRE" >/dev/null 2>&1 || true
docker run -d --name "$NOPRE" -e POSTGRES_HOST_AUTH_METHOD=trust "$IMAGE" -c idgenkit.machine_id=9 >/dev/null
trap 'docker rm -f "$NAME" "$NOPRE" >/dev/null 2>&1 || true' EXIT
np() { docker exec -i "$NOPRE" psql -U postgres -X -q -A -t "$@" 2>&1; }
for _ in $(seq 60); do np -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done
sleep 1
for _ in $(seq 60); do np -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done
np -c "CREATE EXTENSION idgenkit" >/dev/null
result=$(np -c "SELECT count(DISTINCT snowflake_generate()) || ' ' || min(snowflake_machine_id(snowflake_generate())) FROM generate_series(1, 10000)" || true)
if (( PG_MAJOR >= 17 )); then
  [[ "$result" == "10000 9" ]] && echo "PASS snowflake via DSM registry" || { echo "FAIL got: $result"; exit 1; }
else
  [[ "$result" == *"shared_preload_libraries"* ]] && echo "PASS clear error without preload" || { echo "FAIL got: $result"; exit 1; }
fi
[[ "$(np -c "SELECT length(ulid_generate()) + length(nanoid_generate())")" == "47" ]] \
  && echo "PASS ulid/nanoid without preload" || { echo "FAIL ulid/nanoid without preload"; exit 1; }
docker rm -f "$NOPRE" >/dev/null

if [[ "${1:-}" == "--bench" ]]; then
  N=${BENCH_N:-1000000}
  echo "# PostgreSQL $PG_MAJOR, SELECT count(expr) FROM generate_series(1, $N), single session"
  base_ms=""
  for expr in "n" "gen_random_uuid()" "ulid_generate()" "ulid_generate_monotonic()" "ulid_generate_uuid()" \
              "snowflake_generate()" "nanoid_generate()" "nanoid_generate(21, '0123456789abcdef')"; do
    best=""
    for _ in 1 2 3; do
      ms=$(docker exec -i "$NAME" psql -U postgres -X -d postgres -c '\timing on' \
        -c "SELECT count($expr) FROM generate_series(1, $N) n" | awk '/^Time:/ {print $2}')
      best=$(awk -v a="$ms" -v b="$best" 'BEGIN { print (b == "" || a < b) ? a : b }')
    done
    [[ "$expr" == "n" ]] && base_ms=$best
    awk -v e="$expr" -v ms="$best" -v base="$base_ms" -v n="$N" 'BEGIN {
      net = (e == "n") ? ms : ms - base
      label = (e == "n") ? "baseline count(n)" : e
      printf "postgres %-38s %8.1f ns/op %14.0f ops/s  (%.0f ms total)\n", label, net * 1e6 / n, n / (net / 1e3), ms }'
  done
fi
