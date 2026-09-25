#!/usr/bin/env bash
# Runs every benchmark and writes a combined report to bench/results/.
#   ./bench/run_all.sh                 # everything available on this machine
#   SKIP="mysql postgres" ./bench/run_all.sh
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="bench/results/$(date +%Y-%m-%d)-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m).txt"
mkdir -p bench/results
SKIP=" ${SKIP:-} "
has() { command -v "$1" >/dev/null 2>&1; }
want() { [[ "$SKIP" != *" $1 "* ]]; }
section() { printf '\n## %s\n' "$1"; }

{
  echo "# idgenkit benchmarks — $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "# host: $(uname -srm), cpus: $(getconf _NPROCESSORS_ONLN)"
  echo "# Snowflake is capped by design at 4096 ids/ms (>= 244 ns/op per generator)."

  if want c; then section "C core"; make -s -C c bench | grep -E '^(c |#)'; fi
  if want rust && has cargo; then section Rust; (cd rust && cargo bench -q --bench bench 2>/dev/null | grep -E '^(rust|#)'); fi
  if want go && has go; then
    section Go
    (cd go && go test -run '^$' -bench . -benchmem ./... | awk '
      /^pkg:/ { n = split($2, p, "/"); pkg = p[n] }
      /^Benchmark/ {
        split($1, a, "-"); sub(/^Benchmark/, "", a[1])
        printf "go      %-28s %11.1f ns/op %14.0f ops/s  %s B/op %s allocs/op\n", pkg "." a[1], $3, 1e9 / $3, $5, $7 }')
  fi
  if want java && has java; then section Java; make -s -C java bench | grep -E '^(java|#)'; fi
  if want python && has python3; then section Python; python3 python/bench/bench.py; fi
  if want redis && has redis-server; then section Redis; make -s -C redis bench; fi
  if want mysql && has mysqld; then section MySQL; make -s -C mysql bench 2>/dev/null | grep -E '^(mysql|# MySQL)'; fi
  if want postgres && has docker; then section PostgreSQL; ./postgres/test.sh --bench 2>/dev/null | grep -E '^(postgres|# PostgreSQL)'; fi
} 2>&1 | tee "$OUT"

echo
echo "report written to $OUT"
echo "update the tables in README.md and docs/ with: make docs-tables"
