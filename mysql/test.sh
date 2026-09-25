#!/usr/bin/env bash
# Boots a throwaway mysqld (datadir under build/), loads the UDFs and tests them.
#   ./test.sh          run tests
#   ./test.sh --bench  also run BENCHMARK() comparisons
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$PWD/build/mysql-test"
SOCK="$ROOT/mysql.sock"
rm -rf "$ROOT"
mkdir -p "$ROOT/data"

mysqld --no-defaults --initialize-insecure --datadir="$ROOT/data" >"$ROOT/init.log" 2>&1
mysqld --no-defaults --datadir="$ROOT/data" --socket="$SOCK" --skip-networking --mysqlx=OFF \
  --plugin-dir="$PWD/build" --pid-file="$ROOT/mysqld.pid" --log-error="$ROOT/error.log" &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null || true' EXIT

q() { mysql --no-defaults -uroot -S "$SOCK" -N -B -e "$1" test 2>&1; }
for _ in $(seq 100); do
  mysql --no-defaults -uroot -S "$SOCK" -e "SELECT 1" >/dev/null 2>&1 && break
  sleep 0.2
done
mysql --no-defaults -uroot -S "$SOCK" -e "CREATE DATABASE IF NOT EXISTS test"
mysql --no-defaults -uroot -S "$SOCK" test <install.sql

fail=0
expect() { # expect <description> <regex> <actual>
  if [[ "$3" =~ $2 ]]; then echo "PASS $1"; else echo "FAIL $1: got '$3'"; fail=1; fi
}

# MySQL folds table-independent expressions inside aggregates (COUNT(DISTINCT UUID())
# is 1 as well), so per-row behaviour is checked on materialized rows.
q "CREATE TABLE seq (n INT PRIMARY KEY);
   SET SESSION cte_max_recursion_depth = 1000000;
   INSERT INTO seq WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 20000) SELECT n FROM s;"
materialize() { # materialize <expr>: store one value per row of seq in table ids
  q "DROP TABLE IF EXISTS ids; CREATE TABLE ids (n INT PRIMARY KEY, v VARBINARY(1024));
     INSERT INTO ids SELECT n, $1 FROM seq;"
}

expect "ulid_generate format" '^[0-7][0-9A-HJKMNP-TV-Z]{25}$' "$(q "SELECT ulid_generate()")"
materialize "ulid_generate()"
expect "ulid_generate per row" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
expect "ulid binary sorts like text" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT v, LAG(v) OVER (ORDER BY ulid_to_bin(v)) p FROM ids) b WHERE p >= v")"
materialize "ulid_generate_monotonic()"
expect "ulid_generate_monotonic ordered" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT v, LAG(v) OVER (ORDER BY n) p FROM ids) b WHERE p >= v")"
expect "ulid_timestamp anchor" '^1469918176385$' "$(q "SELECT ulid_timestamp('01ARYZ6S41TSV4RRFFQ69G5FAV')")"
expect "ulid_timestamp invalid is NULL" '^NULL$' "$(q "SELECT ulid_timestamp('8ZZZZZZZZZZZZZZZZZZZZZZZZZ')")"
expect "ulid_timestamp NULL" '^NULL$' "$(q "SELECT ulid_timestamp(NULL)")"
expect "ulid binary round trip" '^1 16$' \
  "$(q "SELECT bin_to_ulid(ulid_to_bin(u)) = u, LENGTH(ulid_to_bin(u)) FROM (SELECT ulid_generate() u) t" | tr '\t' ' ')"
materialize "snowflake_generate()"
expect "snowflake per row unique" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
expect "snowflake ordered" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT CAST(v AS UNSIGNED) v, LAG(CAST(v AS UNSIGNED)) OVER (ORDER BY n) p FROM ids) b WHERE p >= v")"
q "CREATE TABLE sf (v BIGINT PRIMARY KEY)"
pids=()
for _ in 1 2 3 4; do q "INSERT INTO sf SELECT snowflake_generate() FROM seq" & pids+=($!); done
wait "${pids[@]}"
expect "snowflake unique across 4 concurrent connections" '^80000$' "$(q "SELECT COUNT(*) FROM sf")"
expect "snowflake machine id / sequence" '^42 [0-9]+$' \
  "$(q "SELECT snowflake_machine_id(id), snowflake_sequence(id) FROM (SELECT snowflake_generate(42) id) t" | tr '\t' ' ')"
expect "snowflake custom epoch timestamp" '^1$' \
  "$(q "SELECT ABS(snowflake_timestamp(snowflake_generate(7, 1600000000000), 1600000000000) - UNIX_TIMESTAMP(NOW(3)) * 1000) < 5000")"
expect "snowflake vector" '^1750000000000 42 7$' \
  "$(q "SELECT snowflake_timestamp(1934266310456418311, 1288834974657), snowflake_machine_id(1934266310456418311), snowflake_sequence(1934266310456418311)" | tr '\t' ' ')"
expect "snowflake bad machine id errors" '^NULL$' "$(q "SELECT snowflake_generate(2000)")"

expect "nanoid default" '^[A-Za-z0-9_-]{21}$' "$(q "SELECT nanoid_generate()")"
expect "nanoid size" '^64$' "$(q "SELECT LENGTH(nanoid_generate(64))")"
expect "nanoid alphabet" '^[0-9a-f]{12}$' "$(q "SELECT nanoid_generate(12, '0123456789abcdef')")"
materialize "nanoid_generate()"
expect "nanoid per row" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
expect "nanoid invalid size is NULL" '^NULL$' "$(q "SELECT nanoid_generate(0)")"
expect "nanoid empty alphabet is NULL" '^NULL$' "$(q "SELECT nanoid_generate(5, '')")"
expect "arity checked" 'ERROR' "$(q "SELECT ulid_generate(1)")"

if [[ "${1:-}" == "--bench" ]]; then
  N=${BENCH_N:-1000000}
  echo "# MySQL $(q "SELECT VERSION()"), BENCHMARK($N, expr), single connection"
  for expr in "UUID()" "ulid_generate()" "ulid_generate_monotonic()" "snowflake_generate()" \
              "nanoid_generate()" "nanoid_generate(21, '0123456789abcdef')"; do
    start=$(python3 -c 'import time; print(time.time_ns())')
    q "SELECT BENCHMARK($N, $expr)" >/dev/null
    end=$(python3 -c 'import time; print(time.time_ns())')
    awk -v e="$expr" -v t="$(( end - start ))" -v n="$N" \
      'BEGIN { printf "mysql   %-36s %8.1f ns/op %14.0f ops/s\n", e, t / n, n / (t / 1e9) }'
  done
fi

exit $fail
