#!/usr/bin/env bash
# Builds the extension inside postgres:<major> (Debian) and writes a tarball with
# lib/idgenkit.so and extension/idgenkit{.control,--*.sql}.
#   PG_MAJOR=17 ./postgres/package.sh dist/
# Install: copy lib/* to $(pg_config --pkglibdir) and extension/* to
#          $(pg_config --sharedir)/extension.
set -euo pipefail
cd "$(dirname "$0")/.."

PG_MAJOR=${PG_MAJOR:-17}
OUT=${1:-dist}
VERSION=$(sed -n "s/^default_version = '\(.*\)'/\1/p" postgres/idgenkit.control)
ARCH=$(uname -m | sed 's/aarch64/arm64/; s/x86_64/amd64/')
IMAGE="idgenkit-pg:$PG_MAJOR"

mkdir -p "$OUT"
docker build -q -f postgres/Dockerfile --build-arg PG_MAJOR="$PG_MAJOR" -t "$IMAGE" . >/dev/null
docker run --rm --entrypoint bash "$IMAGE" -c '
  set -e
  mkdir -p /tmp/p/lib /tmp/p/extension
  cp "$(pg_config --pkglibdir)/idgenkit.so" /tmp/p/lib/
  cp "$(pg_config --sharedir)"/extension/idgenkit* /tmp/p/extension/
  tar -C /tmp/p -czf - lib extension' \
  > "$OUT/idgenkit-postgres-$VERSION-pg$PG_MAJOR-linux-$ARCH.tar.gz"
echo "$OUT/idgenkit-postgres-$VERSION-pg$PG_MAJOR-linux-$ARCH.tar.gz"
