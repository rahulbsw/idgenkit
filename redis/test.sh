#!/usr/bin/env bash
# Starts a throwaway redis-server with the module loaded and checks every command.
set -euo pipefail
cd "$(dirname "$0")"

PORT=${REDIS_TEST_PORT:-6399}
MODULE="$PWD/build/idgenkit.so"
# Test-only secret; vectors from ../testdata/relid_tag.txt.
IDGENKIT_RELID_SECRET=0123456789abcdef redis-server --port "$PORT" --save "" --appendonly no --daemonize no \
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

UUID_RE='[0-9a-f]{8}-[0-9a-f]{4}-V[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}'
expect "uuidv4.generate format" "^${UUID_RE/V/4}\$" "$(cli uuidv4.generate)"
expect "uuidv7.generate format" "^${UUID_RE/V/7}\$" "$(cli uuidv7.generate)"
expect "uuidv7.time RFC 9562 example" '^1645557742000$' "$(cli uuidv7.time 017F22E2-79B0-7CC3-98C4-DC0C0C07398F)"
now_ms=$(python3 -c 'import time; print(time.time_ns() // 1_000_000)')
ts=$(cli uuidv7.time "$(cli uuidv7.generate)")
expect "uuidv7 timestamp near now" '^1$' "$(( ts > now_ms - 5000 && ts <= now_ms + 5000 ))"
expect "uuidv7.time rejects v4" 'ERR' "$(cli uuidv7.time "$(cli uuidv4.generate)" 2>&1)"
expect "uuidv7.time rejects braces" 'ERR' "$(cli uuidv7.time '{017f22e2-79b0-7cc3-98c4-dc0c0c07398f}' 2>&1)"
mono=$(for _ in $(seq 2000); do echo uuidv7.monotonic; done | cli)
expect "uuidv7.monotonic 2000 increasing and unique" '^2000$' \
  "$(sort -u <<<"$mono" | wc -l | tr -d ' ')$([[ "$(sort <<<"$mono")" == "$mono" ]] || echo ' (out of order)')"
expect "uuidv4 2000 unique" '^2000$' \
  "$(for _ in $(seq 2000); do echo uuidv4.generate; done | cli | sort -u | wc -l | tr -d ' ')"

expect "relid.tag vectors" '^4V81BZ 5731MX $' \
  "$(printf 'relid.tag customer-42\nrelid.tag customer-42 orders\n' | cli | tr '\n' ' ')"
expect "relid.generate format" '^5731MX-[0-7][0-9A-HJKMNP-TV-Z]{9}-[0-9A-HJKMNP-TV-Z]{10}$' \
  "$(cli relid.generate customer-42 orders)"
expect "relid.time vector" '^1469918176385$' "$(cli relid.time 0AQKFF-01ARYZ6S41-RJ6HB7H6NW)"
expect "relid.time bare lowercase" '^1469918176385$' "$(cli relid.time 0aqkff01aryz6s41rj6hb7h6nw)"
expect "relid.time rejects overflow" 'ERR' "$(cli relid.time 0AQKFF-81ARYZ6S41-RJ6HB7H6NW 2>&1)"
relids=$(for _ in $(seq 2000); do echo relid.monotonic customer-42; done | cli)
expect "relid.monotonic 2000 increasing and unique" '^2000$' \
  "$(sort -u <<<"$relids" | wc -l | tr -d ' ')$([[ "$(LC_ALL=C sort <<<"$relids")" == "$relids" ]] || echo ' (out of order)')"
expect "relid.generate 2000 unique" '^2000$' \
  "$(for _ in $(seq 2000); do echo relid.generate k; done | cli | sort -u | wc -l | tr -d ' ')"
expect "relid wrong arity" 'ERR' "$(cli relid.generate 2>&1)"

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

NOSECRET_PORT=$((PORT + 1))
env -u IDGENKIT_RELID_SECRET redis-server --port "$NOSECRET_PORT" --save "" --appendonly no --daemonize no \
  --loadmodule "$MODULE" >build/redis-test-nosecret.log 2>&1 &
NOSECRET_PID=$!
trap 'kill $PID $NOSECRET_PID 2>/dev/null; wait $PID $NOSECRET_PID 2>/dev/null || true' EXIT
for _ in $(seq 50); do redis-cli -p "$NOSECRET_PORT" ping >/dev/null 2>&1 && break; sleep 0.1; done
expect "relid without a secret" 'IDGENKIT_RELID_SECRET' "$(redis-cli -p "$NOSECRET_PORT" relid.generate k 2>&1)"

exit $fail
