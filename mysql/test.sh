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
# Test-only secret; vectors from ../testdata/relid_tag.txt.
IDGENKIT_RELID_SECRET=0123456789abcdef \
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
UUID_RE='[0-9a-f]{8}-[0-9a-f]{4}-V[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}'
expect "uuidv4_generate format" "^${UUID_RE/V/4}\$" "$(q "SELECT uuidv4_generate()")"
expect "uuidv7_generate format" "^${UUID_RE/V/7}\$" "$(q "SELECT uuidv7_generate()")"
materialize "uuidv4_generate()"
expect "uuidv4_generate per row" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
materialize "uuidv7_generate()"
expect "uuidv7_generate per row" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
materialize "uuidv7_generate_monotonic()"
expect "uuidv7_generate_monotonic ordered" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT v, LAG(v) OVER (ORDER BY n) p FROM ids) b WHERE p >= v")"
expect "uuidv7 UUID_TO_BIN sorts like text" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT v, LAG(v) OVER (ORDER BY UUID_TO_BIN(v)) p FROM ids) b WHERE p >= v")"
expect "uuidv7_timestamp RFC 9562 example" '^1645557742000 1645557742000$' \
  "$(q "SELECT uuidv7_timestamp('017F22E2-79B0-7CC3-98C4-DC0C0C07398F'), uuidv7_timestamp(UUID_TO_BIN('017f22e2-79b0-7cc3-98c4-dc0c0c07398f'))" | tr '\t' ' ')"
expect "uuidv7_timestamp is now" '^1$' \
  "$(q "SELECT ABS(uuidv7_timestamp(uuidv7_generate()) - UNIX_TIMESTAMP(NOW(3)) * 1000) < 5000")"
expect "uuidv7_timestamp of v4 is NULL" '^NULL$' "$(q "SELECT uuidv7_timestamp(uuidv4_generate())")"
expect "uuidv7_timestamp invalid is NULL" '^NULL$' "$(q "SELECT uuidv7_timestamp('{017f22e2-79b0-7cc3-98c4-dc0c0c07398f}')")"

expect "relid_tag vectors" '^4V81BZ 5731MX$' \
  "$(q "SELECT relid_tag('customer-42'), relid_tag('customer-42', 'orders')" | tr '\t' ' ')"
expect "relid_generate format" '^5731MX-[0-7][0-9A-HJKMNP-TV-Z]{9}-[0-9A-HJKMNP-TV-Z]{10}$' \
  "$(q "SELECT relid_generate('customer-42', 'orders')")"
materialize "relid_generate(CONCAT('k', n % 7), IF(n % 2, 'orders', 'invoices'))"
expect "relid_generate per row" '^20000$' "$(q "SELECT COUNT(DISTINCT v) FROM ids")"
expect "relid_generate tags follow key and salt" '^14 14$' \
  "$(q "SELECT COUNT(DISTINCT LEFT(v, 6)), COUNT(DISTINCT CONCAT(n % 7, n % 2)) FROM ids" | tr '\t' ' ')"
expect "relid_generate tag matches relid_tag" '^0$' \
  "$(q "SELECT COUNT(*) FROM ids WHERE LEFT(v, 6) <> relid_tag(CONCAT('k', n % 7), IF(n % 2, 'orders', 'invoices'))")"
materialize "relid_generate_monotonic(IF(n % 3, 'a', 'b'))"
expect "relid_generate_monotonic ordered per key" '^0$' \
  "$(q "SELECT COUNT(*) FROM (SELECT v, LAG(v) OVER (PARTITION BY LEFT(v, 6) ORDER BY n) p FROM ids) b WHERE p >= v")"
expect "relid_timestamp vector" '^1469918176385 1469918176385$' \
  "$(q "SELECT relid_timestamp('0AQKFF-01ARYZ6S41-RJ6HB7H6NW'), relid_timestamp('0aqkff01aryz6s41rj6hb7h6nw')" | tr '\t' ' ')"
expect "relid_timestamp is now" '^1$' \
  "$(q "SELECT ABS(relid_timestamp(relid_generate('k')) - UNIX_TIMESTAMP(NOW(3)) * 1000) < 5000")"
expect "relid_timestamp invalid is NULL" '^NULL$' "$(q "SELECT relid_timestamp('0AQKFF-81ARYZ6S41-RJ6HB7H6NW')")"
expect "relid_generate NULL key" '^NULL$' "$(q "SELECT relid_generate(NULL)")"

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
  for expr in "UUID()" "ulid_generate()" "ulid_generate_monotonic()" "uuidv4_generate()" \
              "uuidv7_generate()" "uuidv7_generate_monotonic()" \
              "relid_generate('customer-42')" "relid_generate_monotonic('customer-42')" "snowflake_generate()" \
              "nanoid_generate()" "nanoid_generate(21, '0123456789abcdef')"; do
    start=$(python3 -c 'import time; print(time.time_ns())')
    q "SELECT BENCHMARK($N, $expr)" >/dev/null
    end=$(python3 -c 'import time; print(time.time_ns())')
    awk -v e="$expr" -v t="$(( end - start ))" -v n="$N" \
      'BEGIN { printf "mysql   %-36s %8.1f ns/op %14.0f ops/s\n", e, t / n, n / (t / 1e9) }'
  done
fi

exit $fail
