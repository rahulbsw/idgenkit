"""How well does a column of each ID type compress?

Builds N IDs per format in generation order, as a steady stream at a fixed
rate with synthetic timestamps (the same clock for every format), writes each
column as fixed-width binary or ASCII text, and compresses it with zlib and
LZMA. This approximates what a columnar file (Parquet, ORC) does with a
general-purpose codec. parquet.py measures real Parquet encodings.

Relative IDs each belong to one of 1,000 keys picked at random, and are also
measured sorted by ID, which groups each key's IDs together.

    python3 bench/storage/compression.py [N]
"""

from __future__ import annotations

import lzma
import os
import random
import sys
import uuid
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python" / "src"))

from idgenkit import ULID, RelativeId, nanoid  # noqa: E402
from idgenkit.relid import MAX_RANDOM, MAX_TIMESTAMP, from_parts  # noqa: E402
from idgenkit.snowflake import compose  # noqa: E402

T0 = 1_780_000_000_000  # fixed start time (ms) so runs are reproducible in shape
EPOCH = 1_288_834_974_657
MACHINE = 7

# Relative IDs: each ID belongs to one of RELID_KEYS keys, picked uniformly.
RELID_KEYS = 1_000
RELID_SEED = 20260925
RELID_SECRET = b"bench-only-secret-0123456789"  # benchmark only; protects nothing


def timestamps(n: int, per_ms: int) -> list[int]:
    return [T0 + i // per_ms for i in range(n)]


def uuid_v4(ts: list[int]) -> bytes:
    return b"".join(uuid.uuid4().bytes for _ in ts)


def uuid_v7(ts: list[int]) -> bytes:
    """RFC 9562 UUIDv7 with random rand_a/rand_b (no sub-millisecond counter)."""
    out = bytearray()
    rnd = os.urandom(10 * len(ts))
    for i, ms in enumerate(ts):
        r = int.from_bytes(rnd[10 * i:10 * i + 10], "big")
        rand_a = r >> 68 & 0xFFF
        rand_b = r & (1 << 62) - 1
        value = ms << 80 | 0x7 << 76 | rand_a << 64 | 0b10 << 62 | rand_b
        out += value.to_bytes(16, "big")
    return bytes(out)


def ulid_binary(ts: list[int], monotonic: bool) -> bytes:
    out = bytearray()
    rnd = os.urandom(10 * len(ts))
    last_ms, last_rand = -1, 0
    for i, ms in enumerate(ts):
        if monotonic and ms == last_ms:
            last_rand += 1
        else:
            last_rand = int.from_bytes(rnd[10 * i:10 * i + 10], "big")
        last_ms = ms
        out += (ms << 80 | last_rand).to_bytes(16, "big")
    return bytes(out)


def ulid_text(ts: list[int]) -> bytes:
    rnd = os.urandom(10 * len(ts))
    return "".join(
        str(ULID.from_parts(ms, int.from_bytes(rnd[10 * i:10 * i + 10], "big")))
        for i, ms in enumerate(ts)
    ).encode()


def nanoid_text(ts: list[int]) -> bytes:
    return "".join(nanoid() for _ in ts).encode()


def uuid_v4_text(ts: list[int]) -> bytes:
    return "".join(str(uuid.uuid4()) for _ in ts).encode()


def snowflake(ts: list[int]) -> bytes:
    out = bytearray()
    seq, last = 0, -1
    for ms in ts:
        seq = seq + 1 if ms == last else 0
        last = ms
        out += compose(ms, MACHINE, seq, EPOCH).to_bytes(8, "big")
    return bytes(out)


def sequence(ts: list[int]) -> bytes:
    return b"".join(i.to_bytes(8, "big") for i in range(1, len(ts) + 1))


def relid_values(ts: list[int], monotonic: bool = False) -> list[int]:
    """128-bit values tag << 98 | ms << 50 | random, in arrival order."""
    gen = RelativeId(RELID_SECRET)
    tags = [gen.tag_value(f"customer-{k}") for k in range(RELID_KEYS)]
    picks = random.Random(RELID_SEED).choices(tags, k=len(ts))
    rnd = os.urandom(7 * len(ts))
    out, last_ms, last_rand = [], -1, 0
    for i, (tag, ms) in enumerate(zip(picks, ts)):
        if monotonic and ms == last_ms:
            last_rand += 1
        else:
            last_rand = int.from_bytes(rnd[7 * i:7 * i + 7], "big") & MAX_RANDOM
        last_ms = ms
        out.append(tag << 98 | ms << 50 | last_rand)
    return out


def relid_binary(ts: list[int], monotonic: bool = False, by_id: bool = False) -> bytes:
    vals = relid_values(ts, monotonic)
    if by_id:
        vals.sort()
    return b"".join(v.to_bytes(16, "big") for v in vals)


def relid_text(ts: list[int], by_id: bool = False) -> bytes:
    vals = relid_values(ts)
    if by_id:
        vals.sort()
    return "".join(from_parts(v >> 98, v >> 50 & MAX_TIMESTAMP, v & MAX_RANDOM) for v in vals).encode()


FORMATS = [
    ("bigint sequence (baseline)", sequence),
    ("snowflake (8 B)", snowflake),
    ("uuid v4 (16 B)", uuid_v4),
    ("uuid v7 (16 B)", uuid_v7),
    ("ulid (16 B)", lambda ts: ulid_binary(ts, monotonic=False)),
    ("ulid monotonic (16 B)", lambda ts: ulid_binary(ts, monotonic=True)),
    ("ulid text (26 chars)", ulid_text),
    ("relid (16 B)", relid_binary),
    ("relid monotonic (16 B)", lambda ts: relid_binary(ts, monotonic=True)),
    ("relid sorted by id (16 B)", lambda ts: relid_binary(ts, by_id=True)),
    ("relid text (28 chars)", relid_text),
    ("relid text sorted by id", lambda ts: relid_text(ts, by_id=True)),
    ("nanoid text (21 chars)", nanoid_text),
    ("uuid v4 text (36 chars)", uuid_v4_text),
]


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
    print(f"# {n:,} IDs per column; bytes per ID: raw / zlib-6 / lzma-6")
    for per_ms, label in ((1, "1,000 IDs/s"), (100, "100,000 IDs/s")):
        ts = timestamps(n, per_ms)
        print(f"\n## steady {label}")
        print(f"{'format':28} {'raw':>6} {'zlib':>6} {'lzma':>6}")
        for name, build in FORMATS:
            data = build(ts)
            z = len(zlib.compress(data, 6)) / n
            x = len(lzma.compress(data, preset=6)) / n
            print(f"{name:28} {len(data) / n:6.1f} {z:6.1f} {x:6.1f}")


if __name__ == "__main__":
    main()
