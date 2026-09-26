"""How big is a Parquet column of each ID type, and which layout helps?

Uses the same generators and synthetic clock as compression.py, writes each
column as a real Parquet file with several encodings, and reports file bytes
per ID. It also writes each column in 100 row groups and counts how many row
groups a point lookup must read after min/max statistics prune the rest, and
for relative IDs, how many a scan for every ID of one key must read.

Needs pyarrow (bench only; the libraries have no dependencies):

    python3 -m venv build/bench-venv
    build/bench-venv/bin/pip install -r bench/storage/requirements.txt
    build/bench-venv/bin/python bench/storage/parquet.py [N]
"""

from __future__ import annotations

import random
import sys

import pyarrow as pa
import pyarrow.parquet as pq

from compression import (
    nanoid_text,
    relid_binary,
    relid_text,
    sequence,
    snowflake,
    timestamps,
    ulid_binary,
    ulid_text,
    uuid_v4,
    uuid_v4_text,
    uuid_v7,
)

ZSTD = {"use_dictionary": False, "compression": "zstd", "compression_level": 3}
ROW_GROUPS = 100
LOOKUPS = 1000
SEED = 20260925


def shuffled(build):
    def run(ts: list[int]) -> bytes:
        data = build(ts)
        width = len(data) // len(ts)
        rows = [data[i:i + width] for i in range(0, len(data), width)]
        random.Random(SEED).shuffle(rows)
        return b"".join(rows)
    return run


# kind: "int64" (8-byte integer), "fixed16" (16 random bytes),
# "timed16" (48-bit millisecond timestamp + 80 bits), "relid16" (30-bit tag +
# 48-bit ms + 50 random bits), "text" (fixed-width ASCII)
FORMATS = [
    ("bigint sequence (baseline)", sequence, "int64"),
    ("snowflake", snowflake, "int64"),
    ("uuid v4", uuid_v4, "fixed16"),
    ("uuid v7", uuid_v7, "timed16"),
    ("uuid v7, rows shuffled", shuffled(uuid_v7), "timed16"),
    ("ulid", lambda ts: ulid_binary(ts, monotonic=False), "timed16"),
    ("ulid monotonic", lambda ts: ulid_binary(ts, monotonic=True), "timed16"),
    ("ulid text", ulid_text, "text"),
    ("relid", relid_binary, "relid16"),
    ("relid monotonic", lambda ts: relid_binary(ts, monotonic=True), "relid16"),
    ("relid, sorted by id", lambda ts: relid_binary(ts, by_id=True), "relid16"),
    ("relid text", relid_text, "text"),
    ("relid text, sorted by id", lambda ts: relid_text(ts, by_id=True), "text"),
    ("nanoid text", nanoid_text, "text"),
    ("uuid v4 text", uuid_v4_text, "text"),
]


def values(kind: str, data: bytes, n: int) -> list:
    width = len(data) // n
    rows = [data[i:i + width] for i in range(0, len(data), width)]
    if kind == "int64":
        return [int.from_bytes(r, "big", signed=True) for r in rows]
    if kind == "text":
        return [r.decode("ascii") for r in rows]
    return rows


def arrow_type(kind: str, width: int) -> pa.DataType:
    if kind == "int64":
        return pa.int64()
    if kind == "text":
        return pa.string()
    return pa.binary(width)


def layouts(kind: str, vals: list):
    """Yields (layout name, table, write options)."""
    width = 8 if kind == "int64" else len(vals[0])
    t = pa.table({"id": pa.array(vals, arrow_type(kind, width))})
    yield "defaults (dictionary, snappy)", t, {}
    yield "plain + zstd", t, ZSTD
    if kind == "int64":
        yield "delta + zstd", t, {**ZSTD, "column_encoding": {"id": "DELTA_BINARY_PACKED"}}
    elif kind == "text":
        yield "delta + zstd", t, {**ZSTD, "column_encoding": {"id": "DELTA_BYTE_ARRAY"}}
    else:
        yield "byte_stream_split + zstd", t, {**ZSTD, "column_encoding": {"id": "BYTE_STREAM_SPLIT"}}
    if kind == "timed16":
        split = pa.table({
            "ms": pa.array([int.from_bytes(v[:6], "big") for v in vals], pa.int64()),
            "rest": pa.array([v[6:] for v in vals], pa.binary(10)),
        })
        yield "split: ms delta + rest zstd", split, {
            **ZSTD, "column_encoding": {"ms": "DELTA_BINARY_PACKED", "rest": "PLAIN"},
        }
    if kind == "relid16":
        ints = [int.from_bytes(v, "big") for v in vals]
        split = pa.table({
            "tag": pa.array([v >> 98 for v in ints], pa.int32()),
            "ms": pa.array([v >> 50 & (1 << 48) - 1 for v in ints], pa.int64()),
            "rest": pa.array([(v & (1 << 50) - 1).to_bytes(7, "big") for v in ints], pa.binary(7)),
        })
        yield "split: tag dict + ms delta + rest zstd", split, {
            **ZSTD, "use_dictionary": ["tag"],
            "column_encoding": {"ms": "DELTA_BINARY_PACKED", "rest": "PLAIN"},
        }


def file_bytes(table: pa.Table, **options) -> int:
    sink = pa.BufferOutputStream()
    pq.write_table(table, sink, **options)
    return sink.getvalue().size


def row_group_ranges(kind: str, vals: list) -> list[tuple]:
    """(min, max) statistics of each of ROW_GROUPS row groups."""
    width = 8 if kind == "int64" else len(vals[0])
    t = pa.table({"id": pa.array(vals, arrow_type(kind, width))})
    sink = pa.BufferOutputStream()
    pq.write_table(t, sink, row_group_size=len(vals) // ROW_GROUPS, **ZSTD)
    meta = pq.ParquetFile(pa.BufferReader(sink.getvalue())).metadata
    return [
        (s.min, s.max)
        for s in (meta.row_group(g).column(0).statistics for g in range(meta.num_row_groups))
    ]


def row_groups_per_lookup(kind: str, vals: list) -> float:
    ranges = row_group_ranges(kind, vals)
    probes = random.Random(SEED).sample(vals, LOOKUPS)
    hits = sum(lo <= p <= hi for p in probes for lo, hi in ranges)
    return hits / LOOKUPS


def relid_tag(kind: str, value) -> int | str:
    """The part of a relative ID that is the same for every ID of one key."""
    return int.from_bytes(value, "big") >> 98 if kind == "relid16" else value[:6]


def row_groups_per_key_scan(kind: str, vals: list) -> float:
    """Row groups a scan for every ID of one key reads after min/max pruning."""
    ranges = [(relid_tag(kind, lo), relid_tag(kind, hi)) for lo, hi in row_group_ranges(kind, vals)]
    keys = random.Random(SEED).sample(sorted({relid_tag(kind, v) for v in vals}), 100)
    return sum(lo <= k <= hi for k in keys for lo, hi in ranges) / len(keys)


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
    print(f"# {n:,} IDs per column; Parquet file bytes per ID "
          f"(pyarrow {pa.__version__}, zstd level 3, one row group)")
    for per_ms, label in ((1, "1,000 IDs/s"), (100, "100,000 IDs/s")):
        ts = timestamps(n, per_ms)
        print(f"\n## steady {label}")
        print(f"{'format':28}| {'layout':38}| B/ID")
        cols = {}
        for name, build, kind in FORMATS:
            vals = values(kind, build(ts), n)
            cols[name] = (kind, vals)
            for layout, table, options in layouts(kind, vals):
                print(f"{name:28}| {layout:38}| {file_bytes(table, **options) / n:.2f}")
        if per_ms == 1:
            print(f"\n## row groups read per point lookup (of {ROW_GROUPS}), steady {label}")
            print(f"{'format':28}| row groups")
            for name, (kind, vals) in cols.items():
                print(f"{name:28}| {row_groups_per_lookup(kind, vals):.1f}")
            print(f"\n## row groups read to scan every ID of one key (of {ROW_GROUPS}), steady {label}")
            print(f"{'format':28}| row groups")
            for name, (kind, vals) in cols.items():
                if name.startswith("relid"):
                    print(f"{name:28}| {row_groups_per_key_scan(kind, vals):.1f}")


if __name__ == "__main__":
    main()
