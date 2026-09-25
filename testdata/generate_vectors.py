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


# The generator behaviour vectors below come from these reference models of the
# rules in docs/ALGORITHMS.md, not from any implementation, so every port
# (including the Python one) is checked against the specification.

ULID_MAX_TIME = (1 << 48) - 1
ULID_RANDOM_MAX = (1 << 80) - 1


class MonotonicUlidModel:
    def __init__(self) -> None:
        self.last = None

    def draws_random(self, now: int) -> bool:
        return self.last is None or now > self.last >> 80

    def next(self, now: int, random: int):
        """Returns the next ULID value, or None for an error (state unchanged)."""
        if not self.draws_random(now):
            if self.last & ULID_RANDOM_MAX == ULID_RANDOM_MAX:
                return None
            self.last += 1
            return self.last
        if now > ULID_MAX_TIME:
            return None
        self.last = now << 80 | random
        return self.last


def ulid_monotonic_vectors() -> None:
    anchor = ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
    t = anchor.timestamp_ms
    # ("reset",) or (now_ms, random or None); None draws from the fixed stream.
    cases = [
        ("reset",),
        (t, anchor.randomness),
        (t, None),  # same millisecond: previous + 1
        (t - 5, None),  # clock moved backwards: keep the last timestamp
        (t + 1, None),  # new millisecond: fresh randomness
        ("reset",),
        (1000, 0x0000000000FFFFFFFFFF),
        (1000, None),  # carry into the next byte
        (1000, None),
        ("reset",),
        (2000, ULID_RANDOM_MAX - 1),
        (2000, None),  # reaches the maximum random value
        (2000, None),  # overflow
        (1999, None),  # still overflowing, clock behind
        (2001, None),  # recovers in the next millisecond
        ("reset",),
        (ULID_MAX_TIME + 1, 0),  # beyond 48 bits
        (ULID_MAX_TIME, None),  # the failed call left no state behind
        (ULID_MAX_TIME, None),
        (ULID_MAX_TIME + 1, None),  # beyond 48 bits after a valid ID
        (ULID_MAX_TIME, None),  # continues from the last valid ID
    ]
    rows, model, draw = [], MonotonicUlidModel(), 0
    for case in cases:
        if case[0] == "reset":
            rows.append(("reset",))
            model = MonotonicUlidModel()
            continue
        now, random = case
        if model.draws_random(now):
            if random is None:
                random = int.from_bytes(stream(f"monotonic{draw}", 10), "big")
                draw += 1
            random_hex = f"{random:020x}"
        else:
            random, random_hex = 0, "-"
        value = model.next(now, random)
        rows.append(("next", now, random_hex, "error" if value is None else str(ULID(value))))
    write(
        "ulid_monotonic.txt",
        "# Monotonic ULID generator behaviour. \"reset\" starts a new generator.\n"
        "#   next <now_ms> <random_hex> <expected>\n"
        "# random_hex: the 10 bytes the random source returns if the call draws randomness,\n"
        "#             or \"-\" if the call must not draw any.\n"
        "# expected:   the canonical ULID, or \"error\" (the call fails and the generator is unchanged).\n",
        rows,
    )


UUID_MAX_TIME = (1 << 48) - 1
UUID7_RANDOM_MAX = (1 << 74) - 1


def uuid_with_version(raw: bytes, version: int) -> bytes:
    b = bytearray(raw)
    b[6] = (b[6] & 0x0F) | (version << 4)
    b[8] = (b[8] & 0x3F) | 0x80
    return bytes(b)


def uuid_text(b: bytes) -> str:
    h = b.hex()
    return f"{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}"


def uuid4_model(random16: bytes) -> str:
    return uuid_text(uuid_with_version(random16, 4))


def uuid7_bytes(ms: int, random10: bytes) -> bytes:
    return uuid_with_version(ms.to_bytes(6, "big") + random10, 7)


def uuid7_rand(b: bytes) -> int:
    """The 74 random bits of a UUIDv7: rand_a << 62 | rand_b."""
    v = int.from_bytes(b, "big")
    return ((v >> 64) & 0xFFF) << 62 | (v & ((1 << 62) - 1))


def uuid7_from_rand(ms: int, rand: int) -> bytes:
    v = ms << 80 | 0x7 << 76 | (rand >> 62) << 64 | 0b10 << 62 | (rand & ((1 << 62) - 1))
    return v.to_bytes(16, "big")


def uuid_vectors() -> None:
    rows = []
    # RFC 9562 appendix A.3 and A.6.
    rfc_v4 = bytes.fromhex("919108f752d143209bacf847db4148a8")
    rfc_v7 = bytes.fromhex("017f22e279b07cc398c4dc0c0c07398f")
    v4_cases = [rfc_v4, bytes(16), b"\xff" * 16] + [stream(f"uuid4-{i}", 16) for i in range(6)]
    for raw in v4_cases:
        rows.append(("v4", raw.hex(), uuid4_model(raw)))
    v7_cases = [
        (int.from_bytes(rfc_v7[:6], "big"), rfc_v7[6:]),
        (0, bytes(10)),
        (UUID_MAX_TIME, b"\xff" * 10),
        (1469918176385, stream("uuid7-anchor", 10)),
    ] + [(int.from_bytes(stream(f"uuid7-ms{i}", 6), "big"), stream(f"uuid7-{i}", 10)) for i in range(6)]
    for ms, raw in v7_cases:
        rows.append(("v7", ms, raw.hex(), uuid_text(uuid7_bytes(ms, raw))))
    for ms in (UUID_MAX_TIME + 1, 1 << 62):
        rows.append(("v7", ms, bytes(10).hex(), "error"))
    write(
        "uuid.txt",
        "# UUIDv4 and UUIDv7 from fixed randomness (docs/ALGORITHMS.md).\n"
        "#   v4 <random_hex(16 bytes)> <expected>\n"
        "#   v7 <timestamp_ms> <random_hex(10 bytes)> <expected>\n"
        "# expected: the canonical lowercase text form, or \"error\" (timestamp out of range).\n",
        rows,
    )


def uuid_invalid_vectors() -> None:
    valid = "017f22e2-79b0-7cc3-98c4-dc0c0c07398f"
    cases = [
        "",
        valid[:-1],
        valid + "0",
        valid.replace("-", ""),
        "{" + valid + "}",
        "urn:uuid:" + valid,
        valid[:8] + "_" + valid[9:],
        valid[:13] + valid[14:] + "0",
        "017f22e2-79b0-7cc3-98c4-dc0c0c07398g",
        "017f22e2-79b0-7cc3-98c4-dc0c0c07398 ",
        " 17f22e2-79b0-7cc3-98c4-dc0c0c07398f",
        "017f22e2-79b0-7cc3-98c4+dc0c0c07398f",
        "017f22e279b0-7cc3-98c4-dc0c-0c07398f",
    ]
    with open(os.path.join(HERE, "uuid_invalid.txt"), "w") as f:
        f.write("# Strings every UUID parser must reject, one per line between the quotes.\n")
        for case in cases:
            f.write(f'"{case}"\n')


class MonotonicUuid7Model:
    def __init__(self) -> None:
        self.last_ms = None
        self.last_rand = 0

    def draws_random(self, now: int) -> bool:
        return self.last_ms is None or now > self.last_ms

    def next(self, now: int, random10: bytes):
        """Returns the next UUIDv7 bytes, or None for an error (state unchanged)."""
        if not self.draws_random(now):
            if self.last_rand == UUID7_RANDOM_MAX:
                return None
            self.last_rand += 1
            return uuid7_from_rand(self.last_ms, self.last_rand)
        if now > UUID_MAX_TIME:
            return None
        b = uuid7_bytes(now, random10)
        self.last_ms, self.last_rand = now, uuid7_rand(b)
        return b


def uuid7_monotonic_vectors() -> None:
    t = 0x017F22E279B0
    rfc_random = bytes.fromhex("7cc398c4dc0c0c07398f")
    # ("reset",) or (now_ms, random10 or None); None draws from the fixed stream.
    cases = [
        ("reset",),
        (t, rfc_random),
        (t, None),  # same millisecond: previous + 1
        (t - 5, None),  # clock moved backwards: keep the last timestamp
        (t + 1, None),  # new millisecond: fresh randomness
        ("reset",),
        (1000, bytes.fromhex("0000ffffffffffffffff")),  # rand_b at its maximum
        (1000, None),  # carry from rand_b into rand_a, skipping the variant bits
        (1000, None),
        ("reset",),
        (2000, bytes.fromhex("ffffffffffffffffffff")),  # every random bit set
        (2000, None),  # overflow
        (1999, None),  # still overflowing, clock behind
        (2001, None),  # recovers in the next millisecond
        ("reset",),
        (UUID_MAX_TIME + 1, bytes(10)),  # beyond 48 bits
        (UUID_MAX_TIME, None),  # the failed call left no state behind
        (UUID_MAX_TIME, None),
        (UUID_MAX_TIME + 1, None),  # beyond 48 bits after a valid ID
        (UUID_MAX_TIME, None),  # continues from the last valid ID
    ]
    rows, model, draw = [], MonotonicUuid7Model(), 0
    for case in cases:
        if case[0] == "reset":
            rows.append(("reset",))
            model = MonotonicUuid7Model()
            continue
        now, random10 = case
        if model.draws_random(now):
            if random10 is None:
                random10 = stream(f"uuid7-monotonic{draw}", 10)
                draw += 1
            random_hex = random10.hex()
        else:
            random10, random_hex = bytes(10), "-"
        value = model.next(now, random10)
        rows.append(("next", now, random_hex, "error" if value is None else uuid_text(value)))
    write(
        "uuid7_monotonic.txt",
        "# Monotonic UUIDv7 generator behaviour. \"reset\" starts a new generator.\n"
        "#   next <now_ms> <random_hex> <expected>\n"
        "# random_hex: the 10 bytes the random source returns if the call draws randomness,\n"
        "#             or \"-\" if the call must not draw any.\n"
        "# expected:   the canonical UUID, or \"error\" (the call fails and the generator is unchanged).\n",
        rows,
    )


SF_MAX_SEQUENCE = 4095
SF_MAX_DELTA = (1 << 42) - 1


class SnowflakeModel:
    def __init__(self, machine_id: int, epoch_ms: int) -> None:
        self.machine_id, self.epoch_ms = machine_id, epoch_ms
        self.last_ms, self.seq = 0, 0

    def next(self, readings: list):
        """Returns the next id, or None for an error (state unchanged)."""
        clock = iter(readings)
        last_reading = readings[-1]

        def read() -> int:
            return next(clock, last_reading)

        while True:
            now = read()
            if now > self.last_ms:
                ms, seq = now, 0
            elif self.seq < SF_MAX_SEQUENCE:
                ms, seq = self.last_ms, self.seq + 1
            else:
                while read() <= self.last_ms:
                    pass
                continue
            if not 0 <= ms - self.epoch_ms <= SF_MAX_DELTA:
                return None
            self.last_ms, self.seq = ms, seq
            return (ms - self.epoch_ms) << 22 | self.machine_id << 12 | seq


def snowflake_sequence_vectors() -> None:
    epoch, t = 1288834974657, 1_750_000_000_000
    # ("gen", machine, epoch), ("next", [readings]) or ("fill", count, clock)
    cases = [
        ("gen", 7, epoch),
        ("next", [t]),
        ("next", [t]),  # same millisecond: sequence 1
        ("next", [t + 1]),  # new millisecond: sequence 0
        ("next", [t - 5]),  # clock moved backwards: keep the last millisecond
        ("fill", 4094, t + 1),  # uses up sequences 2..4095
        ("next", [t + 1, t + 1, t + 2]),  # exhausted: waits for the next millisecond
        ("next", [t + 2]),
        ("gen", 3, epoch),
        ("next", [t]),
        ("fill", 4095, t),
        ("next", [t - 3, t - 3, t, t + 1]),  # exhausted with the clock behind
        ("gen", 0, epoch),
        ("next", [epoch - 1]),  # before the epoch
        ("next", [epoch]),
        ("gen", 1, epoch),
        ("next", [epoch + SF_MAX_DELTA + 1]),  # beyond 42 bits
        ("next", [epoch + 5]),  # the failed call left no state behind
        ("gen", 1023, 0),
        ("next", [t]),
    ]
    rows, model = [], None
    for case in cases:
        if case[0] == "gen":
            model = SnowflakeModel(case[1], case[2])
            rows.append(case)
        elif case[0] == "fill":
            for _ in range(case[1]):
                assert model.next([case[2]]) is not None
            rows.append(case)
        else:
            value = model.next(case[1])
            rows.append(("next", ",".join(map(str, case[1])), "error" if value is None else value))
    write(
        "snowflake_sequence.txt",
        "# Snowflake generator behaviour. \"gen\" starts a new generator.\n"
        "#   gen <machine_id> <epoch_ms>\n"
        "#   next <clock_ms[,clock_ms...]> <expected>\n"
        "#   fill <count> <clock_ms>\n"
        "# next: the clock returns the listed readings in order, then repeats the last one.\n"
        "#       expected is the id, or \"error\" (the call fails and the generator is unchanged).\n"
        "# fill: calls next <count> times with a fixed clock; each call succeeds with a larger id.\n",
        rows,
    )


if __name__ == "__main__":
    ulid_vectors()
    snowflake_vectors()
    nanoid_vectors()
    ulid_monotonic_vectors()
    snowflake_sequence_vectors()
    uuid_vectors()
    uuid_invalid_vectors()
    uuid7_monotonic_vectors()
