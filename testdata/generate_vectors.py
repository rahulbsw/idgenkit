#!/usr/bin/env python3
"""Regenerate the shared conformance vectors consumed by every implementation.

Vectors are deterministic: randomness comes from a fixed SHA-256 based stream.
Run from the repository root:  python3 testdata/generate_vectors.py
"""

import hashlib
import hmac
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


def hmac_sha256_vectors() -> None:
    # RFC 4231 test cases 1-4, 6 and 7 (case 5 is truncated), then lengths
    # around the 64-byte block boundary for keys and messages.
    rfc = [
        (b"\x0b" * 20, b"Hi There"),
        (b"Jefe", b"what do ya want for nothing?"),
        (b"\xaa" * 20, b"\xdd" * 50),
        (bytes(range(1, 26)), b"\xcd" * 50),
        (b"\xaa" * 131, b"Test Using Larger Than Block-Size Key - Hash Key First"),
        (
            b"\xaa" * 131,
            b"This is a test using a larger than block-size key and a larger than block-size data. "
            b"The key needs to be hashed before being used by the HMAC algorithm.",
        ),
    ]
    expected_rfc1 = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
    cases = list(rfc)
    for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 1000):
        cases.append((stream(f"hmac-key{n % 3}", 32), stream(f"hmac-msg{n}", n)))
    for n in (0, 16, 63, 64, 65, 200):
        cases.append((stream(f"hmac-longkey{n}", n), b"abc"))
    rows = []
    for key, msg in cases:
        rows.append((key.hex() or "-", msg.hex() or "-", hmac.new(key, msg, hashlib.sha256).hexdigest()))
    assert rows[0][2] == expected_rfc1
    write(
        "hmac_sha256.txt",
        "# HMAC-SHA-256 (RFC 2104 / FIPS 180-4), for implementations without one built in.\n"
        "#   <key_hex> <message_hex> <mac_hex>      \"-\" stands for empty.\n",
        rows,
    )


RELID_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
RELID_MAX_TAG = (1 << 30) - 1
RELID_MAX_TIME = (1 << 48) - 1
RELID_MAX_RANDOM = (1 << 50) - 1


def relid_field(value: int, width: int) -> str:
    return "".join(RELID_ALPHABET[(value >> (5 * (width - 1 - i))) & 31] for i in range(width))


def relid_text(tag: int, ms: int, rand: int) -> str:
    return f"{relid_field(tag, 6)}-{relid_field(ms, 10)}-{relid_field(rand, 10)}"


def relid_tag(secret: bytes, salt: bytes, key: bytes) -> int:
    mac = hmac.new(secret, len(salt).to_bytes(4, "big") + salt + key, hashlib.sha256).digest()
    return int.from_bytes(mac[:4], "big") >> 2


def relid_tag_vectors() -> None:
    s16 = b"0123456789abcdef"
    s32 = stream("relid-secret", 32)
    cases = [
        (s16, b"", b""),
        (s16, b"", b"customer-42"),
        (s16, b"orders", b"customer-42"),
        (s16, b"invoices", b"customer-42"),
        # The length prefix keeps these two apart.
        (s16, b"ab", b"c"),
        (s16, b"a", b"bc"),
        (s32, b"", b"DSABAAadaasdas"),
        (s32, "t\u00e9nant".encode(), "cl\u00e9-\u65e5\u672c-\U0001f600".encode()),
        (stream("relid-long-secret", 100), b"x", b"y"),
    ]
    for n in (55, 56, 60, 64, 119, 120, 200):
        cases.append((s32, b"salt", "k".encode() * n))
    rows = []
    for secret, salt, key in cases:
        tag = relid_tag(secret, salt, key)
        rows.append((secret.hex(), salt.hex() or "-", key.hex() or "-", tag, relid_field(tag, 6)))
    write(
        "relid_tag.txt",
        "# Relative ID tags (docs/ALGORITHMS.md). Salt and key are UTF-8; \"-\" stands for empty.\n"
        "#   <secret_hex> <salt_hex> <key_hex> <tag> <tag_text>\n",
        rows,
    )


def relid_vectors() -> None:
    rows = []
    cases = [
        (0, 0, 0),
        (RELID_MAX_TAG, RELID_MAX_TIME, RELID_MAX_RANDOM),
        (1, 1469918176385, 1),
        (1 << 29, 1 << 47, 1 << 49),
    ]
    for i in range(6):
        r = int.from_bytes(stream(f"relid{i}", 16), "big")
        cases.append((r >> 98, (r >> 50) & RELID_MAX_TIME, r & RELID_MAX_RANDOM))
    for tag, ms, rand in cases:
        rows.append(("parts", tag, ms, rand, relid_text(tag, ms, rand)))
    for tag, ms, rand in (
        (RELID_MAX_TAG + 1, 0, 0),
        (0, RELID_MAX_TIME + 1, 0),
        (0, 0, RELID_MAX_RANDOM + 1),
    ):
        rows.append(("parts", tag, ms, rand, "error"))
    # Alternative spellings that parse to the same value.
    tag, ms, rand = cases[4]
    text = relid_text(tag, ms, rand)
    for alt in (text.lower(), text.replace("-", ""), text.replace("-", "").lower()):
        rows.append(("parse", alt, tag, ms, rand))
    write(
        "relid.txt",
        "# Relative ID text form (docs/ALGORITHMS.md).\n"
        "#   parts <tag> <timestamp_ms> <random> <expected>   expected is the text, or \"error\".\n"
        "#   parse <text> <tag> <timestamp_ms> <random>       a non-canonical spelling that must parse.\n",
        rows,
    )


def relid_invalid_vectors() -> None:
    valid = relid_text(0x1234567, 1469918176385, 0x2ABCDEF012345)
    bare = valid.replace("-", "")
    cases = [
        "",
        valid[:-1],
        valid + "0",
        bare[:-1],
        bare + "0",
        valid[:6] + valid[7:],  # only one hyphen
        valid[:6] + "_" + valid[7:],
        valid[:17] + "_" + valid[18:],
        valid[:5] + "-" + valid[5] + valid[7:],
        valid[:6] + bare[6:16] + "-" + valid[18:] + "0",
        valid[:7] + "8" + valid[8:],  # time overflows 48 bits
        bare[:6] + "8" + bare[7:],
        valid[:3] + "I" + valid[4:],
        valid[:3] + "L" + valid[4:],
        valid[:3] + "O" + valid[4:],
        valid[:3] + "U" + valid[4:],
        valid[:-1] + " ",
        " " + valid[1:],
        valid[:10] + "\u00e9" + valid[11:],
    ]
    with open(os.path.join(HERE, "relid_invalid.txt"), "w", encoding="utf-8") as f:
        f.write("# Strings every relative ID parser must reject, one per line between the quotes.\n")
        for case in cases:
            f.write(f'"{case}"\n')


class MonotonicRelidModel:
    def __init__(self) -> None:
        self.last_ms = None
        self.last_rand = 0

    def draws_random(self, now: int) -> bool:
        return self.last_ms is None or now > self.last_ms

    def next(self, tag: int, now: int, random7: bytes):
        """Returns the next ID text, or None for an error (state unchanged)."""
        if not self.draws_random(now):
            if self.last_rand == RELID_MAX_RANDOM:
                return None
            self.last_rand += 1
        else:
            if now > RELID_MAX_TIME:
                return None
            self.last_ms, self.last_rand = now, int.from_bytes(random7, "big") & RELID_MAX_RANDOM
        return relid_text(tag, self.last_ms, self.last_rand)


def relid_monotonic_vectors() -> None:
    t = 1469918176385
    a, b = 0x0ABCDEF, 0x3FFFFFF
    # ("reset",) or (tag, now_ms, random7 or None); None draws from the fixed stream.
    cases = [
        ("reset",),
        (a, t, bytes.fromhex("ff123456789abc")),  # the top 6 bits are masked off
        (a, t, None),  # same millisecond: previous + 1
        (b, t, None),  # another key shares the counter
        (a, t - 5, None),  # clock moved backwards: keep the last timestamp
        (b, t + 1, None),  # new millisecond: fresh randomness
        ("reset",),
        (a, 2000, bytes.fromhex("03fffffffffffe")),
        (b, 2000, None),  # reaches the maximum
        (a, 2000, None),  # overflow
        (a, 1999, None),  # still overflowing, clock behind
        (a, 2001, None),  # recovers in the next millisecond
        ("reset",),
        (a, RELID_MAX_TIME + 1, bytes(7)),  # beyond 48 bits
        (a, RELID_MAX_TIME, None),  # the failed call left no state behind
        (b, RELID_MAX_TIME, None),
        (a, RELID_MAX_TIME + 1, None),  # beyond 48 bits after a valid ID
        (a, RELID_MAX_TIME, None),  # continues from the last valid ID
    ]
    rows, model, draw = [], MonotonicRelidModel(), 0
    for case in cases:
        if case[0] == "reset":
            rows.append(("reset",))
            model = MonotonicRelidModel()
            continue
        tag, now, random7 = case
        if model.draws_random(now):
            if random7 is None:
                random7 = stream(f"relid-monotonic{draw}", 7)
                draw += 1
            random_hex = random7.hex()
        else:
            random7, random_hex = bytes(7), "-"
        value = model.next(tag, now, random7)
        rows.append(("next", tag, now, random_hex, "error" if value is None else value))
    write(
        "relid_monotonic.txt",
        "# Monotonic relative ID generator behaviour. \"reset\" starts a new generator.\n"
        "#   next <tag> <now_ms> <random_hex> <expected>\n"
        "# random_hex: the 7 bytes the random source returns if the call draws randomness,\n"
        "#             or \"-\" if the call must not draw any.\n"
        "# expected:   the ID text, or \"error\" (the call fails and the generator is unchanged).\n",
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
    hmac_sha256_vectors()
    relid_tag_vectors()
    relid_vectors()
    relid_invalid_vectors()
    relid_monotonic_vectors()
