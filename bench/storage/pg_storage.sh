#!/usr/bin/env bash
# Storage cost of each ID type as a PostgreSQL primary key.
# For every case: create a table with that key, insert N rows in one statement
# (after a CHECKPOINT), then report insert time, heap/index size, B-tree leaf
# density and WAL written. Runs in the postgres:18 image built by postgres/Dockerfile.
#   N=5000000 ./bench/storage/pg_storage.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

N=${N:-5000000}
PG_MAJOR=18
IMAGE="idgenkit-pg:$PG_MAJOR"
NAME=idgenkit-storage-bench

docker build -q -f postgres/Dockerfile --build-arg PG_MAJOR=$PG_MAJOR -t "$IMAGE" . >/dev/null
docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" -e POSTGRES_HOST_AUTH_METHOD=trust "$IMAGE" \
  -c shared_preload_libraries=idgenkit -c idgenkit.machine_id=7 \
  -c idgenkit.snowflake_epoch_ms=1288834974657 -c max_wal_size=8GB >/dev/null
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT

q() { docker exec -i "$NAME" psql -U postgres -X -q -A -t -v ON_ERROR_STOP=1 "$@"; }
for _ in $(seq 60); do q -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done
sleep 1
for _ in $(seq 60); do q -c 'SELECT 1' >/dev/null 2>&1 && break; sleep 0.5; done
q -c "CREATE EXTENSION IF NOT EXISTS idgenkit; CREATE EXTENSION IF NOT EXISTS pgstattuple" >/dev/null

# label | column definition | value expression ("" = use the column default)
CASES=(
  'bigint identity (baseline)|bigint GENERATED ALWAYS AS IDENTITY|'
  'snowflake bigint|bigint|snowflake_generate()'
  'uuid v4 (gen_random_uuid)|uuid|gen_random_uuid()'
  'uuid v7 (uuidv7)|uuid|uuidv7()'
  'ulid as uuid|uuid|ulid_generate_uuid()'
  'ulid monotonic as uuid|uuid|ulid_to_uuid(ulid_generate_monotonic())'
  'ulid text COLLATE "C"|text COLLATE "C"|ulid_generate()'
  'ulid monotonic text COLLATE "C"|text COLLATE "C"|ulid_generate_monotonic()'
  'nanoid(21) text COLLATE "C"|text COLLATE "C"|nanoid_generate()'
  'uuid v4 as text|text|gen_random_uuid()::text'
)

q -c "CREATE TABLE warmup (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v int NOT NULL);
      INSERT INTO warmup (v) SELECT g FROM generate_series(1, $N) g;
      CREATE TABLE warmup2 AS SELECT gen_random_uuid(), uuidv7(), ulid_generate(), snowflake_generate(), nanoid_generate() FROM generate_series(1, $N / 10);
      DROP TABLE warmup, warmup2"

echo "# PostgreSQL $PG_MAJOR, $N rows per table, one INSERT ... SELECT, shared_buffers=$(q -c 'SHOW shared_buffers')"
printf '%-32s|%10s|%10s|%10s|%12s|%10s|%10s\n' case insert_ms heap_MB index_MB leaf_density wal_MB index_B/row
for c in "${CASES[@]}"; do
  IFS='|' read -r label coldef expr <<<"$c"
  q -c "DROP TABLE IF EXISTS t; CREATE TABLE t (id $coldef PRIMARY KEY, v int NOT NULL)"
  q -c "CHECKPOINT"
  lsn0=$(q -c "SELECT pg_current_wal_lsn()")
  if [[ -z "$expr" ]]; then ins="INSERT INTO t (v) SELECT g FROM generate_series(1, $N) g"
  else ins="INSERT INTO t (id, v) SELECT $expr, g FROM generate_series(1, $N) g"; fi
  ms=$(q -c "DO \$\$ DECLARE t0 timestamptz := clock_timestamp(); BEGIN $ins; RAISE NOTICE '%', round(extract(epoch FROM clock_timestamp() - t0) * 1000); END \$\$" 2>&1 | sed -n 's/.*NOTICE: *//p')
  q -c "SELECT '$label', $ms,
          round(pg_relation_size('t') / 1048576.0, 1),
          round(pg_relation_size('t_pkey') / 1048576.0, 1),
          (SELECT avg_leaf_density FROM pgstatindex('t_pkey')),
          round(pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn0') / 1048576.0, 1),
          round(pg_relation_size('t_pkey')::numeric / $N, 1)" \
    | awk -F'|' '{ printf "%-32s|%10s|%10s|%10s|%11s%%|%10s|%10s\n", $1, $2, $3, $4, $5, $6, $7 }'
done
