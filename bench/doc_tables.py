"""Rewrites the benchmark tables in README.md and docs/*.md from bench/results/.

    python3 bench/doc_tables.py          # update the tables in place
    python3 bench/doc_tables.py --check  # exit 1 if any table is out of date

Each table sits between `<!-- bench-table NAME -->` and `<!-- /bench-table -->`.
Library and database timings come from the newest dated report written by
`make bench`; storage tables come from bench/results/storage-*.txt.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from storage.charts import RESULTS, ROOT, compression_rows, parquet_rows, postgres_rows

REPO = "https://github.com/rahulbsw/idgenkit"
DOCS = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]
BLOCK = re.compile(r"<!-- bench-table (\S+) -->\n.*?<!-- /bench-table -->", re.S)
MEASUREMENT = re.compile(r"^(\w+)\s+(.+?)\s+([\d.]+) (?:ns/op|req/s)")
VERSION = re.compile(r"^# (Java|Python|PostgreSQL|MySQL|Redis) ([\d.]+)")

LIBRARY_COLUMNS = [
    "ULID", "ULID monotonic", "ULID parse", "UUIDv4", "UUIDv7", "UUIDv7 monotonic", "Relative ID", "Snowflake",
    "Nano ID (21)",
]
SHARED_BENCHMARKS = ["uuid.v4", "uuid.v7", "uuid.v7.monotonic", "relid.generate"]
LIBRARY_BENCHMARKS = {
    "c": ("C core", ["ulid.new", "ulid.monotonic", "ulid.decode", *SHARED_BENCHMARKS, "snowflake.next", "nanoid(21)"]),
    "rust": ("Rust", [
        "ulid.new", "ulid.monotonic", "ulid.parse", *SHARED_BENCHMARKS, "snowflake.next_id", "nanoid(21)",
    ]),
    "go": ("Go", [
        "ulid.New", "ulid.Monotonic", "ulid.Parse", "uuid.NewV4", "uuid.NewV7", "uuid.Monotonic", "relid.Generate",
        "snowflake.Next", "nanoid.New",
    ]),
    "java": ("Java", [
        "ulid.generate", "ulid.monotonic", "ulid.parse", *SHARED_BENCHMARKS, "snowflake.nextId", "nanoid(21)",
    ]),
    "python": ("Python", [
        "ulid.ULID.generate", "ulid.monotonic", "ulid.parse", *SHARED_BENCHMARKS, "snowflake.next_id", "nanoid(21)",
    ]),
}

DATABASE_COLUMNS = [
    "ULID", "ULID monotonic", "ULID as `uuid`", "UUIDv7", "UUIDv7 monotonic", "Relative ID", "Snowflake", "Nano ID",
    "Built-in reference",
]
DATABASE_BENCHMARKS = {
    "postgres": ("PostgreSQL", "ns per row", [
        "ulid_generate()", "ulid_generate_monotonic()", "ulid_generate_uuid()",
        "uuidv7_generate()", "uuidv7_generate_monotonic()", "relid_generate('customer-42')",
        "snowflake_generate()", "nanoid_generate()", "gen_random_uuid()",
    ]),
    "mysql": ("MySQL", "ns per call", [
        "ulid_generate()", "ulid_generate_monotonic()", None,
        "uuidv7_generate()", "uuidv7_generate_monotonic()", "relid_generate('customer-42')",
        "snowflake_generate()", "nanoid_generate()", "UUID()",
    ]),
    "redis": ("Redis", "requests/s", [
        "ULID.GENERATE", "ULID.MONOTONIC", None, "UUIDV7.GENERATE", "UUIDV7.MONOTONIC", "RELID.GENERATE customer-42",
        "SNOWFLAKE.GENERATE", "NANOID.GENERATE", "PING",
    ]),
}

POSTGRES_LABELS = {
    "bigint identity (baseline)": "`bigint` identity (baseline)",
    "snowflake bigint": "Snowflake `bigint`",
    "uuid v7 (uuidv7)": "UUIDv7 `uuid` (`uuidv7()`)",
    "ulid monotonic as uuid": "ULID monotonic as `uuid`",
    "ulid as uuid": "ULID (plain) as `uuid`",
    "uuid v4 (gen_random_uuid)": "UUIDv4 `uuid` (`gen_random_uuid()`)",
    'ulid monotonic text COLLATE "C"': 'ULID monotonic `text COLLATE "C"`',
    'ulid text COLLATE "C"': 'ULID (plain) `text COLLATE "C"`',
    'nanoid(21) text COLLATE "C"': 'Nano ID `text COLLATE "C"`',
    "uuid v4 as text": "UUIDv4 as `text`",
}

COMPRESSION_LABELS = {
    "bigint sequence (baseline)": "`bigint` sequence (baseline)",
    "snowflake (8 B)": "Snowflake",
    "uuid v7 (16 B)": "UUIDv7, 16 bytes",
    "ulid (16 B)": "ULID, 16 bytes",
    "ulid monotonic (16 B)": "ULID monotonic, 16 bytes",
    "ulid text (26 chars)": "ULID text, 26 chars",
    "relid (16 B)": "Relative ID, 16 bytes",
    "relid monotonic (16 B)": "Relative ID monotonic, 16 bytes",
    "relid sorted by id (16 B)": "Relative ID sorted by ID, 16 bytes",
    "relid text (28 chars)": "Relative ID text, 28 chars",
    "relid text sorted by id": "Relative ID text sorted by ID",
    "uuid v4 (16 B)": "UUIDv4, 16 bytes",
    "nanoid text (21 chars)": "Nano ID text, 21 chars",
    "uuid v4 text (36 chars)": "UUIDv4 text, 36 chars",
}

# Floor = the format's random bits / 8: UUIDv7 74, ULID 80, UUIDv4 122, Nano ID 126,
# Relative ID 50 plus log2(1,000) bits for which of the benchmark's 1,000 keys it belongs to.
PARQUET_LABELS = {
    "bigint sequence (baseline)": ("`bigint` sequence (baseline)", "0"),
    "snowflake": ("Snowflake `INT64`", "about 0"),
    "uuid v7": ("UUIDv7, 16 bytes", "9.25"),
    "uuid v7, rows shuffled": ("UUIDv7, rows shuffled", "9.25"),
    "ulid": ("ULID, 16 bytes", "10"),
    "ulid text": ("ULID text, 26 chars", "10"),
    "relid": ("Relative ID, 16 bytes", "7.5"),
    "relid, sorted by id": ("Relative ID, sorted by ID", "7.5"),
    "relid text": ("Relative ID text, 28 chars", "7.5"),
    "relid text, sorted by id": ("Relative ID text, sorted by ID", "7.5"),
    "uuid v4": ("UUIDv4, 16 bytes", "15.25"),
    "nanoid text": ("Nano ID text, 21 chars", "15.75"),
    "uuid v4 text": ("UUIDv4 text, 36 chars", "15.25"),
}
PARQUET_LAYOUTS = {
    "defaults (dictionary, snappy)": "defaults",
    "plain + zstd": "plain",
    "delta + zstd": "delta",
    "byte_stream_split + zstd": "byte stream split",
    "split: ms delta + rest zstd": "split column",
    "split: tag dict + ms delta + rest zstd": "split columns",
}
NEAR_FLOOR_BYTES = 0.2
BAD_COMPRESSED_BYTES = 15


def warn(text: str) -> str:
    return f'<span class="warn">{text}</span>'


def bad(text: str) -> str:
    return f'<span class="bad">{text}</span>'


def whole(value: float) -> str:
    return f"{int(value + 0.5):,}"


def table(header: list[str], rows: list[list[str]]) -> str:
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


def latest_report() -> Path:
    reports = sorted(RESULTS.glob("[0-9][0-9][0-9][0-9]-*.txt"))
    if not reports:
        sys.exit("no dated report in bench/results/; run `make bench` first")
    return reports[-1]


def read_report(path: Path) -> tuple[dict[str, dict[str, float]], dict[str, str]]:
    """({tool: {benchmark: value}}, {tool name: short version})."""
    values: dict[str, dict[str, float]] = {}
    versions: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if m := VERSION.match(line):
            parts = m.group(2).split(".")[:2]
            while len(parts) > 1 and parts[-1] == "0":
                parts.pop()
            versions[m.group(1)] = ".".join(parts)
        elif m := MEASUREMENT.match(line):
            values.setdefault(m.group(1), {})[m.group(2)] = float(m.group(3))
    return values, versions


def lookup(values: dict[str, dict[str, float]], tool: str, name: str, path: Path) -> float:
    try:
        return values[tool][name]
    except KeyError:
        sys.exit(f"{path.relative_to(ROOT)} has no `{tool} {name}` result; rerun `make bench` with it enabled")


def labelled(name: str, versions: dict[str, str]) -> str:
    return f"{name} {versions[name]}" if name in versions else name


def libraries_table() -> tuple[str, Path]:
    path = latest_report()
    values, versions = read_report(path)
    rows = [
        [labelled(name, versions), *(whole(lookup(values, tool, b, path)) for b in benchmarks)]
        for tool, (name, benchmarks) in LIBRARY_BENCHMARKS.items()
    ]
    return table(["", *LIBRARY_COLUMNS], rows), path


def databases_table() -> tuple[str, Path]:
    path = latest_report()
    values, versions = read_report(path)
    rows = []
    for tool, (name, unit, benchmarks) in DATABASE_BENCHMARKS.items():
        cells = []
        for b in benchmarks:
            if b is None:
                cells.append("—")
                continue
            v = lookup(values, tool, b, path)
            cells.append(f"{whole(v / 1000)}k" if unit == "requests/s" else whole(v))
        cells[-1] = f"`{benchmarks[-1]}` {cells[-1]}"
        rows.append([f"{labelled(name, versions)} ({unit})", *cells])
    return table(["", *DATABASE_COLUMNS], rows), path


def postgres_storage_table() -> tuple[str, Path]:
    by_case = {r["case"]: r for r in postgres_rows()}
    baseline_ms = int(by_case["bigint identity (baseline)"]["insert_ms"])
    rows = []
    for case, label in POSTGRES_LABELS.items():
        r = by_case[case]
        insert_ms, density = int(r["insert_ms"]), float(r["leaf_density"].rstrip("%"))
        insert = f"{insert_ms / 1000:.1f} s"
        leaf = f"{density:.0f}%"
        rows.append([
            label,
            bad(insert) if insert_ms > 2 * baseline_ms else insert,
            f'{whole(float(r["heap_MB"]))} MB',
            f'{whole(float(r["index_MB"]))} MB',
            f'{float(r["index_B/row"]):.1f} B',
            warn(leaf) if density < 80 else leaf,
            f'{whole(float(r["wal_MB"]))} MB',
        ])
    header = ["ID type", "Insert 5M rows", "Table", "Index", "Index per row", "Leaf pages full", "WAL"]
    return table(header, rows), RESULTS / "storage-postgres18.txt"


def compression_table() -> tuple[str, Path]:
    slow = {r[0]: r for r in compression_rows("1,000 IDs/s")}
    fast = {r[0]: r for r in compression_rows("100,000 IDs/s")}

    def cell(v: float) -> str:
        return bad(f"{v:.1f}") if v >= BAD_COMPRESSED_BYTES else f"{v:.1f}"

    rows = [
        [label, f"{slow[fmt][1]:g}", *(cell(v) for v in (*slow[fmt][2:], *fast[fmt][2:]))]
        for fmt, label in COMPRESSION_LABELS.items()
    ]
    header = ["Format", "Raw", "1,000/s zlib", "1,000/s LZMA", "100,000/s zlib", "100,000/s LZMA"]
    return table(header, rows), RESULTS / "storage-compression.txt"


def row_groups_per_lookup() -> dict[str, float]:
    text = (RESULTS / "storage-parquet.txt").read_text()
    block = text.split("## row groups read per point lookup")[1].split("##")[0]
    _heading, _header, *lines = block.strip().splitlines()
    return {fmt.strip(): float(groups) for fmt, groups in (line.split("|") for line in lines)}


def parquet_table() -> tuple[str, Path]:
    sizes, groups = parquet_rows("1,000 IDs/s"), row_groups_per_lookup()
    rows = []
    for fmt, (label, floor) in PARQUET_LABELS.items():
        layouts = sizes[fmt]
        layout, size = min(layouts.items(), key=lambda kv: kv[1])
        plain = layouts["plain + zstd"]
        if size >= plain:
            best = f"{size:.2f} (nothing helps)"
        elif size - float(floor.split()[-1]) <= NEAR_FLOOR_BYTES:
            best = f"**{size:.2f}** {PARQUET_LAYOUTS[layout]}"
        else:
            best = f"{size:.2f} {PARQUET_LAYOUTS[layout]}"
        lookups = f"{groups[fmt]:g}"
        rows.append([
            label,
            f'{layouts["defaults (dictionary, snappy)"]:.2f}',
            f"{plain:.2f}",
            best,
            floor,
            bad(lookups) if groups[fmt] > 1 else lookups,
        ])
    header = ["Format", "pyarrow defaults", "Plain + zstd", "Best layout", "Floor", "Row groups per lookup"]
    return table(header, rows), RESULTS / "storage-parquet.txt"


TABLES = {
    "libraries": libraries_table,
    "databases": databases_table,
    "postgres-storage": postgres_storage_table,
    "compression": compression_table,
    "parquet": parquet_table,
}


def render(name: str) -> str:
    if name not in TABLES:
        sys.exit(f"unknown bench-table {name!r}; known: {', '.join(TABLES)}")
    body, source = TABLES[name]()
    rel = source.relative_to(ROOT).as_posix()
    return (
        f"<!-- bench-table {name} -->\n\n{body}\n\n"
        f"Source: [`{rel}`]({REPO}/blob/main/{rel}), generated by `bench/doc_tables.py`.\n\n"
        "<!-- /bench-table -->"
    )


def main() -> None:
    check = "--check" in sys.argv[1:]
    stale = []
    for doc in DOCS:
        old = doc.read_text()
        new = BLOCK.sub(lambda m: render(m.group(1)), old)
        if new != old:
            stale.append(doc.relative_to(ROOT).as_posix())
            if not check:
                doc.write_text(new)
    if not check:
        print("updated:", ", ".join(stale) or "nothing")
    elif stale:
        sys.exit(f"benchmark tables out of date in {', '.join(stale)}; run `python3 bench/doc_tables.py`")
    else:
        print("benchmark tables up to date")


if __name__ == "__main__":
    main()
