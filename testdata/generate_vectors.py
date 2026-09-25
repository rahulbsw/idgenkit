#!/usr/bin/env python3
"""Regenerate the shared conformance vectors consumed by every implementation.

Vectors are deterministic: randomness comes from a fixed SHA-256 based stream.
Run from the repository root:  python3 testdata/generate_vectors.py
"""

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python", "src"))

from idgenkit import ULID, custom_random, URL_ALPHABET  # noqa: E402
from idgenkit import snowflake  # noqa: E402


def stream(seed: str, n: int) -> bytes:
    out = b""
    counter = 0
    while len(out) < n:
        out += hashlib.sha256(f"{seed}:{counter}".encode()).digest()
        counter += 1
    return out[:n]


def write(name: str, header: str, rows: list) -> None:
    with open(os.path.join(HERE, name), "w") as f:
        f.write(header)
        for row in rows:
            f.write(" ".join(str(c) for c in row) + "\n")


def ulid_vectors() -> None:
    rows = []
    cases = [
        (0, 0),
        ((1 << 48) - 1, (1 << 80) - 1),
        # Anchor from the ulid/javascript README: 01ARYZ6S41 <-> 1469918176385.
        (1469918176385, ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAV").randomness),
        (1, 1),
        (1 << 47, 1 << 79),
    ]
    for i in range(8):
        r = stream(f"ulid{i}", 16)
        cases.append((int.from_bytes(r[:6], "big"), int.from_bytes(r[6:], "big")))
    for ts, rnd in cases:
        rows.append((ts, f"{rnd:020x}", str(ULID.from_parts(ts, rnd))))
    write("ulid.txt", "# timestamp_ms randomness_hex(80-bit) canonical_ulid\n", rows)


def snowflake_vectors() -> None:
    rows = []
    cases = [
        (0, 0, 0, 0),
        (0, 1, 0, 1_700_000_000_000),
        (0, 1023, 4095, 1_700_000_000_123),
        (1288834974657, 42, 7, 1_750_000_000_000),
        (1577836800000, 512, 1, 1_577_836_800_000),
        (0, 1023, 4095, (1 << 42) - 1),
    ]
    for epoch, mid, seq, ts in cases:
        rows.append((epoch, mid, seq, ts, snowflake.compose(ts, mid, seq, epoch)))
    write("snowflake.txt", "# epoch_ms machine_id sequence timestamp_ms id\n", rows)


def nanoid_vectors() -> None:
    rows = []
    cases = [
        (URL_ALPHABET, 21),
        (URL_ALPHABET, 5),
        ("abc", 10),
        ("0123456789abcdef", 12),
        ("0123456789", 30),
        ("x", 4),
        ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 16),
    ]
    for i, (alphabet, size) in enumerate(cases):
        data = stream(f"nanoid{i}", 512)
        pos = 0

        def source(n: int) -> bytes:
            nonlocal pos
            chunk = data[pos : pos + n]
            if len(chunk) != n:
                raise RuntimeError("vector stream exhausted")
            pos += n
            return chunk

        expected = custom_random(alphabet, size, source)()
        rows.append((alphabet, size, data.hex(), expected))
    write("nanoid.txt", "# alphabet size random_stream_hex expected\n", rows)


if __name__ == "__main__":
    ulid_vectors()
    snowflake_vectors()
    nanoid_vectors()
