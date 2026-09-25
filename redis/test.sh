#!/usr/bin/env bash
# Starts a throwaway redis-server with the module loaded and checks every command.
set -euo pipefail
cd "$(dirname "$0")"

PORT=${REDIS_TEST_PORT:-6399}
MODULE="$PWD/build/idgenkit.so"
redis-server --port "$PORT" --save "" --appendonly no --daemonize no \
  --loadmodule "$MODULE" MACHINE_ID 42 EPOCH_MS 1600000000000 >build/redis-test.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null || true' EXIT

cli() { redis-cli -p "$PORT" "$@"; }
for _ in $(seq 50); do cli ping >/dev/null 2>&1 && break; sleep 0.1; done

fail=0
expect() { # expect <description> <regex> <actual>
  if [[ "$3" =~ $2 ]]; then echo "PASS $1"; else echo "FAIL $1: got '$3'"; fail=1; fi
}

expect "ulid.generate format" '^[0-7][0-9A-HJKMNP-TV-Z]{25}$' "$(cli ulid.generate)"
expect "ulid.time anchor" '^1469918176385$' "$(cli ulid.time 01ARYZ6S41TSV4RRFFQ69G5FAV)"
expect "ulid.time lowercase" '^1469918176385$' "$(cli ulid.time 01aryz6s41tsv4rrffq69g5fav)"
expect "ulid.time rejects overflow" 'ERR' "$(cli ulid.time 8ZZZZZZZZZZZZZZZZZZZZZZZZZ 2>&1)"

a=$(cli ulid.monotonic); b=$(cli ulid.monotonic)
expect "ulid.monotonic increasing" '^yes$' "$([[ "$b" > "$a" ]] && echo yes || echo "no ($a >= $b)")"

id=$(cli snowflake.generate)
expect "snowflake.generate integer" '^[0-9]+$' "$id"
parts=$(cli snowflake.parse "$id" | tr '\n' ' ')
expect "snowflake.parse machine id" '^[0-9]+ 42 [0-9]+ $' "$parts"
now_ms=$(python3 -c 'import time; print(time.time_ns() // 1_000_000)')
ts=${parts%% *}
expect "snowflake timestamp near now" '^1$' "$(( ts > now_ms - 5000 && ts <= now_ms + 5000 ))"
expect "snowflake.parse vector" '^1750000000000 42 7 $' \
  "$(cli snowflake.parse $(( ((1750000000000 - 1600000000000) << 22) | (42 << 12) | 7 )) | tr '\n' ' ')"

uniq_count=$(for _ in $(seq 2000); do echo snowflake.generate; done | cli | sort -u | wc -l | tr -d ' ')
expect "snowflake 2000 unique" '^2000$' "$uniq_count"

expect "nanoid default" '^[A-Za-z0-9_-]{21}$' "$(cli nanoid.generate)"
expect "nanoid size" '^[A-Za-z0-9_-]{64}$' "$(cli nanoid.generate 64)"
expect "nanoid alphabet" '^[0-9a-f]{12}$' "$(cli nanoid.generate 12 0123456789abcdef)"
expect "nanoid rejects size 0" 'ERR' "$(cli nanoid.generate 0 2>&1)"
expect "nanoid rejects empty alphabet" 'ERR' "$(cli nanoid.generate 5 '' 2>&1)"
expect "wrong arity" 'ERR' "$(cli ulid.generate extra 2>&1)"

exit $fail
