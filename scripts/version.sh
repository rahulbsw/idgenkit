#!/usr/bin/env bash
# Keeps the published package versions in lockstep.
#   scripts/version.sh show           # print the version of every manifest
#   scripts/version.sh check 0.2.0    # fail unless every manifest says 0.2.0
#   scripts/version.sh set 0.2.0      # rewrite every manifest to 0.2.0
set -euo pipefail
cd "$(dirname "$0")/.."

POM=java/pom.xml
PYPROJECT=python/pyproject.toml
PYINIT=python/src/idgenkit/__init__.py
CARGO=rust/Cargo.toml

versions() {
  printf '%s %s\n' \
    "$POM"       "$(awk -F'[<>]' '/<version>/ { print $3; exit }' "$POM")" \
    "$PYPROJECT" "$(sed -n 's/^version = "\(.*\)"/\1/p' "$PYPROJECT" | head -1)" \
    "$PYINIT"    "$(sed -n 's/^__version__ = "\(.*\)"/\1/p' "$PYINIT")" \
    "$CARGO"     "$(sed -n 's/^version = "\(.*\)"/\1/p' "$CARGO" | head -1)"
}

case "${1:-show}" in
  show)
    versions ;;
  check)
    want="${2:?usage: $0 check <version>}"
    bad=$(versions | awk -v w="$want" '$2 != w')
    if [[ -n "$bad" ]]; then
      echo "version mismatch, expected $want:" >&2
      echo "$bad" >&2
      exit 1
    fi
    echo "all manifests at $want" ;;
  set)
    new="${2:?usage: $0 set <version>}"
    [[ "$new" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$ ]] || { echo "not a version: $new" >&2; exit 1; }
    tmp=$(mktemp)
    awk -v v="$new" '!done && /<version>/ { sub(/<version>[^<]*<\/version>/, "<version>" v "</version>"); done = 1 } 1' "$POM" > "$tmp" && cat "$tmp" > "$POM"
    for f in "$PYPROJECT" "$CARGO"; do
      awk -v v="$new" '!done && /^version = "/ { $0 = "version = \"" v "\""; done = 1 } 1' "$f" > "$tmp" && cat "$tmp" > "$f"
    done
    awk -v v="$new" '/^__version__ = "/ { $0 = "__version__ = \"" v "\"" } 1' "$PYINIT" > "$tmp" && cat "$tmp" > "$PYINIT"
    rm -f "$tmp"
    (cd rust && cargo update -p idgenkit --offline >/dev/null 2>&1 || true)
    versions ;;
  *)
    echo "usage: $0 show | check <version> | set <version>" >&2; exit 2 ;;
esac
