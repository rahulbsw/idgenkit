#!/usr/bin/env python3
"""Micro-benchmarks for the pure-Python implementation (stdlib timeit only)."""

import os
import platform
import sys
import timeit

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from idgenkit import (  # noqa: E402
    ULID,
    MonotonicULID,
    MonotonicUUID7,
    Snowflake,
    custom_alphabet,
    nanoid,
    uuid4,
    uuid7,
)
from idgenkit import ulid  # noqa: E402

N = int(os.environ.get("BENCH_N", "200000"))


def bench(name, fn):
    fn()
    best = min(timeit.repeat(fn, number=N, repeat=3))
    ns = best / N * 1e9
    print(f"python  {name:<28} {ns:10.1f} ns/op {1e9 / ns:14,.0f} ops/s")


def main():
    print(f"# Python {platform.python_version()} ({platform.machine()}), N={N}")
    mono = MonotonicULID()
    uuid_mono = MonotonicUUID7()
    sf = Snowflake(machine_id=1)
    hex_gen = custom_alphabet("0123456789abcdef", 21)
    sample = ulid.generate()

    bench("ulid.generate(str)", ulid.generate)
    bench("ulid.ULID.generate", ULID.generate)
    bench("ulid.monotonic", mono.next)
    bench("ulid.parse", lambda: ULID.parse(sample))
    bench("uuid.v4", uuid4)
    bench("uuid.v7", uuid7)
    bench("uuid.v7.monotonic", uuid_mono.next)
    bench("snowflake.next_id", sf.next_id)
    bench("nanoid(21)", nanoid)
    bench("nanoid.custom(hex,21)", hex_gen)


if __name__ == "__main__":
    main()
