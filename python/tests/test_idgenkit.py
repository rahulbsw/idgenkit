import os
import re
import sys
import threading
import time
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))
TESTDATA = os.path.join(HERE, "..", "..", "testdata")

from idgenkit import (  # noqa: E402
    ULID,
    URL_ALPHABET,
    MonotonicULID,
    MonotonicUUID7,
    RelativeId,
    Snowflake,
    custom_alphabet,
    custom_random,
    nanoid,
    uuid4,
    uuid7,
    uuid7_timestamp,
)
from idgenkit import relid, snowflake, ulid, uuids  # noqa: E402


def vectors(name):
    with open(os.path.join(TESTDATA, name)) as f:
        return [line.split() for line in f if line.strip() and not line.startswith("#")]


class ULIDTest(unittest.TestCase):
    def test_vectors(self):
        for ts, rnd, text in vectors("ulid.txt"):
            u = ULID.from_parts(int(ts), int(rnd, 16))
            self.assertEqual(str(u), text)
            parsed = ULID.parse(text)
            self.assertEqual(parsed.timestamp_ms, int(ts))
            self.assertEqual(parsed.randomness, int(rnd, 16))
            self.assertEqual(ULID.parse(text.lower()), parsed)
            self.assertEqual(ULID.from_bytes(parsed.to_bytes()), parsed)
            self.assertEqual(ULID.from_uuid(parsed.to_uuid()), parsed)

    def test_invalid(self):
        for (text,) in vectors("ulid_invalid.txt"):
            with self.assertRaises(ValueError, msg=text):
                ULID.parse(text)
        with self.assertRaises(ValueError):
            ULID.parse("")
        with self.assertRaises(ValueError):
            ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAé")

    def test_generate(self):
        text = ulid.generate()
        self.assertRegex(text, r"^[0-7][0-9A-HJKMNP-TV-Z]{25}$")
        self.assertIsInstance(ULID.generate().to_uuid(), uuid.UUID)
        self.assertEqual(len({ulid.generate() for _ in range(10000)}), 10000)

    def test_from_parts_range(self):
        with self.assertRaises(ValueError):
            ULID.from_parts(1 << 48, 0)
        with self.assertRaises(ValueError):
            ULID.from_parts(0, 1 << 80)

    def test_monotonic(self):
        gen = MonotonicULID()
        ids = [gen.next() for _ in range(20000)]
        self.assertEqual(ids, sorted(ids))
        self.assertEqual(len(set(ids)), len(ids))
        strs = [str(i) for i in ids]
        self.assertEqual(strs, sorted(strs))

    def test_monotonic_overflow(self):
        gen = MonotonicULID()
        gen._last_ms = 1 << 47  # far future, forces the "same millisecond" path
        gen._last_rand = (1 << 80) - 1
        with self.assertRaises(OverflowError):
            gen.next()
        with self.assertRaises(OverflowError):
            gen.next()

    def test_monotonic_threads(self):
        gen = MonotonicULID()
        out = []
        lock = threading.Lock()

        def work():
            local = [gen.next() for _ in range(5000)]
            with lock:
                out.extend(local)

        threads = [threading.Thread(target=work) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len(set(out)), 20000)

    def test_monotonic_vectors(self):
        rows = vectors("ulid_monotonic.txt")
        self.assertGreater(len(rows), 10)
        gen = MonotonicULID()
        for row in rows:
            if row[0] == "reset":
                gen = MonotonicULID()
                continue
            _, now, rnd, want = row
            drew = []

            def random(n, rnd=rnd, drew=drew):
                drew.append(n)
                return bytes(n) if rnd == "-" else bytes.fromhex(rnd)

            if want == "error":
                with self.assertRaises((ValueError, OverflowError), msg=row):
                    gen._next_at(int(now), random)
            else:
                self.assertEqual(str(gen._next_at(int(now), random)), want, row)
            self.assertFalse(rnd == "-" and drew, f"{row}: drew randomness")


def scripted_clock(readings):
    values = [int(v) for v in readings.split(",")]

    def clock():
        return values.pop(0) if len(values) > 1 else values[0]

    return clock


class UUIDTest(unittest.TestCase):
    def test_vectors(self):
        rows = vectors("uuid.txt")
        self.assertGreater(len(rows), 10)
        for row in rows:
            kind, want = row[0], row[-1]
            if want == "error":
                with self.assertRaises(ValueError, msg=row):
                    uuids.uuid7_from_parts(int(row[1]), bytes.fromhex(row[2]))
                continue
            if kind == "v4":
                u = uuids.uuid4_from_bytes(bytes.fromhex(row[1]))
            else:
                u = uuids.uuid7_from_parts(int(row[1]), bytes.fromhex(row[2]))
            self.assertEqual(str(u), want)
            self.assertEqual(uuids.parse(want), u)
            self.assertEqual(uuids.parse(want.upper()), u)
            self.assertEqual((u.version, u.variant), (int(kind[1]), uuid.RFC_4122))
            if kind == "v7":
                self.assertEqual(uuid7_timestamp(u), int(row[1]))
            else:
                with self.assertRaises(ValueError):
                    uuid7_timestamp(u)

    def test_invalid(self):
        with open(os.path.join(TESTDATA, "uuid_invalid.txt")) as f:
            rows = [line.rstrip("\n")[1:-1] for line in f if not line.startswith("#")]
        self.assertGreater(len(rows), 10)
        for text in rows:
            with self.assertRaises(ValueError, msg=repr(text)):
                uuids.parse(text)
        with self.assertRaises(ValueError):
            uuids.parse("017f22e2-79b0-7cc3-98c4-dc0c0c07398\uff10")

    def test_generate(self):
        now = int(time.time() * 1000)
        values = [f() for _ in range(10000) for f in (uuid4, uuid7)]
        self.assertEqual(len(set(values)), 20000)
        self.assertTrue(all(isinstance(u, uuid.UUID) and u.variant == uuid.RFC_4122 for u in values))
        self.assertEqual({u.version for u in values}, {4, 7})
        self.assertLess(abs(uuid7_timestamp(uuid7()) - now), 5000)
        with self.assertRaises(ValueError):
            uuids.uuid7_from_parts(-1, bytes(10))

    def test_monotonic(self):
        gen = MonotonicUUID7()
        ids = [gen.next() for _ in range(50000)]
        self.assertTrue(all(a.bytes < b.bytes for a, b in zip(ids, ids[1:])))
        self.assertEqual([str(u) for u in ids], sorted(str(u) for u in ids))
        seen, threads = [], []
        for _ in range(8):
            t = threading.Thread(target=lambda: seen.extend(gen.next() for _ in range(5000)))
            threads.append(t)
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len(set(seen)), 40000)

    def test_monotonic_vectors(self):
        rows = vectors("uuid7_monotonic.txt")
        self.assertGreater(len(rows), 10)
        gen = MonotonicUUID7()
        for row in rows:
            if row[0] == "reset":
                gen = MonotonicUUID7()
                continue
            now, rnd, want = int(row[1]), row[2], row[3]
            drew = []

            def source(n, rnd=rnd):
                drew.append(n)
                return bytes(n) if rnd == "-" else bytes.fromhex(rnd)

            if want == "error":
                with self.assertRaises((ValueError, OverflowError), msg=row):
                    gen._next_at(now, source)
            else:
                self.assertEqual(str(gen._next_at(now, source)), want, row)
            self.assertFalse(rnd == "-" and drew, f"{row}: drew randomness")


TEST_SECRET = b"test-only-secret-0123456789"


class RelativeIdTest(unittest.TestCase):
    def test_tag_vectors(self):
        rows = vectors("relid_tag.txt")
        self.assertGreater(len(rows), 10)
        for secret, salt, key, tag, tag_text in rows:
            salt = "" if salt == "-" else bytes.fromhex(salt).decode()
            key = "" if key == "-" else bytes.fromhex(key).decode()
            gen = RelativeId(bytes.fromhex(secret), salt)
            self.assertEqual(gen.tag_value(key), int(tag))
            self.assertEqual(gen.tag(key), tag_text)

    def test_secret(self):
        with self.assertRaises(ValueError):
            RelativeId(b"0123456789abcde")
        with self.assertRaises(ValueError):
            RelativeId("0123456789abcdef")  # a str, not bytes

    def test_vectors(self):
        rows = vectors("relid.txt")
        self.assertGreater(len(rows), 10)
        for row in rows:
            if row[0] == "parts":
                tag, ms, rnd, want = int(row[1]), int(row[2]), int(row[3]), row[4]
                if want == "error":
                    with self.assertRaises(ValueError, msg=row):
                        relid.from_parts(tag, ms, rnd)
                    continue
                self.assertEqual(relid.from_parts(tag, ms, rnd), want)
                self.assertEqual(relid.parse(want), (tag, ms, rnd))
            else:
                self.assertEqual(relid.parse(row[1]), (int(row[2]), int(row[3]), int(row[4])))

    def test_invalid(self):
        with open(os.path.join(TESTDATA, "relid_invalid.txt"), encoding="utf-8") as f:
            rows = [line.rstrip("\n")[1:-1] for line in f if not line.startswith("#")]
        self.assertGreater(len(rows), 10)
        for text in rows:
            with self.assertRaises(ValueError, msg=repr(text)):
                relid.parse(text)

    def test_monotonic_vectors(self):
        rows = vectors("relid_monotonic.txt")
        self.assertGreater(len(rows), 10)
        gen = RelativeId(TEST_SECRET)
        for row in rows:
            if row[0] == "reset":
                gen = RelativeId(TEST_SECRET)
                continue
            tag, now, rnd, want = int(row[1]), int(row[2]), row[3], row[4]
            drew = []

            def source(n, rnd=rnd):
                drew.append(n)
                return bytes(n) if rnd == "-" else bytes.fromhex(rnd)

            if want == "error":
                with self.assertRaises((ValueError, OverflowError), msg=row):
                    gen._monotonic_at(tag, now, source)
            else:
                self.assertEqual(gen._monotonic_at(tag, now, source), want, row)
            self.assertFalse(rnd == "-" and drew, f"{row}: drew randomness")

    def test_generate(self):
        now = int(time.time() * 1000)
        orders = RelativeId(TEST_SECRET, "orders")
        invoices = RelativeId(TEST_SECRET, "invoices")
        ids = [orders.generate("customer-42") for _ in range(10000)]
        self.assertEqual(len(set(ids)), 10000)
        tag = orders.tag("customer-42")
        self.assertTrue(all(i.startswith(tag + "-") for i in ids))
        self.assertNotEqual(invoices.tag("customer-42"), tag)
        self.assertLess(abs(relid.parse(ids[0]).timestamp_ms - now), 5000)
        self.assertEqual(relid.parse(ids[0]).tag_text, tag)

    def test_monotonic(self):
        gen = RelativeId(TEST_SECRET)
        ids = [gen.monotonic("customer-42" if i % 3 else "customer-7") for i in range(30000)]
        for key in ("customer-42", "customer-7"):
            mine = [i for i in ids if i.startswith(gen.tag(key))]
            self.assertTrue(all(a < b for a, b in zip(mine, mine[1:])))
        seen, threads = [], []
        for _ in range(8):
            t = threading.Thread(target=lambda: seen.extend(gen.monotonic("k") for _ in range(5000)))
            threads.append(t)
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len(set(seen)), 40000)


class SnowflakeTest(unittest.TestCase):
    def test_sequence_vectors(self):
        rows = vectors("snowflake_sequence.txt")
        self.assertGreater(len(rows), 10)
        gen, prev = None, 0
        for row in rows:
            op = row[0]
            if op == "gen":
                gen, prev = Snowflake(machine_id=int(row[1]), epoch_ms=int(row[2])), 0
            elif op == "fill":
                clock = int(row[2])
                for _ in range(int(row[1])):
                    id_ = gen._next_id(lambda: clock)
                    self.assertGreater(id_, prev, row)
                    prev = id_
            elif op == "next":
                if row[2] == "error":
                    with self.assertRaises((ValueError, OverflowError), msg=row):
                        gen._next_id(scripted_clock(row[1]))
                else:
                    prev = gen._next_id(scripted_clock(row[1]))
                    self.assertEqual(prev, int(row[2]), row)
            else:
                self.fail(f"bad line {row}")

    def test_vectors(self):
        for epoch, mid, seq, ts, id_ in vectors("snowflake.txt"):
            epoch, mid, seq, ts, id_ = map(int, (epoch, mid, seq, ts, id_))
            self.assertEqual(snowflake.compose(ts, mid, seq, epoch), id_)
            self.assertEqual(tuple(snowflake.parse(id_, epoch)), (ts, mid, seq))

    def test_validation(self):
        with self.assertRaises(ValueError):
            Snowflake(machine_id=1024)
        with self.assertRaises(ValueError):
            Snowflake(machine_id=-1)
        with self.assertRaises(ValueError):
            snowflake.compose(0, 0, 4096)
        with self.assertRaises(ValueError):
            snowflake.compose(5, 0, 0, epoch_ms=10)

    def test_unique_and_ordered(self):
        gen = Snowflake(machine_id=7, epoch_ms=1_600_000_000_000)
        ids = [gen.next_id() for _ in range(50000)]
        self.assertEqual(ids, sorted(ids))
        self.assertEqual(len(set(ids)), len(ids))
        parts = gen.parse(ids[-1])
        self.assertEqual(parts.machine_id, 7)
        self.assertGreater(parts.timestamp_ms, 1_600_000_000_000)

    def test_clock_backwards(self):
        gen = Snowflake()
        first = gen.next_id()
        gen._last_ms += 5  # pretend the wall clock jumped backwards by 5 ms
        second = gen.next_id()
        self.assertGreater(second, first)

    def test_threads(self):
        gen = Snowflake(machine_id=3)
        out = []
        lock = threading.Lock()

        def work():
            local = [gen.next_id() for _ in range(5000)]
            with lock:
                out.extend(local)

        threads = [threading.Thread(target=work) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len(set(out)), 20000)


class NanoIDTest(unittest.TestCase):
    def test_vectors(self):
        for alphabet, size, stream_hex, expected in vectors("nanoid.txt"):
            data = bytes.fromhex(stream_hex)
            pos = 0

            def source(n):
                nonlocal pos
                chunk = data[pos : pos + n]
                pos += n
                return chunk

            self.assertEqual(custom_random(alphabet, int(size), source)(), expected)

    def test_default(self):
        value = nanoid()
        self.assertEqual(len(value), 21)
        self.assertTrue(re.fullmatch(r"[A-Za-z0-9_-]{21}", value))
        self.assertEqual(len(nanoid(64)), 64)
        self.assertEqual(len({nanoid() for _ in range(10000)}), 10000)
        self.assertEqual(sorted(URL_ALPHABET), sorted(set(URL_ALPHABET)))

    def test_custom(self):
        gen = custom_alphabet("0123456789", 12)
        value = gen()
        self.assertTrue(re.fullmatch(r"[0-9]{12}", value))
        self.assertEqual(len(gen(5)), 5)

    def test_unicode_alphabet(self):
        gen = custom_alphabet("αβγδ", 8)
        self.assertTrue(set(gen()) <= set("αβγδ"))

    def test_distribution(self):
        gen = custom_alphabet("abcdefghij", 1000)
        counts = {}
        for ch in "".join(gen() for _ in range(100)):
            counts[ch] = counts.get(ch, 0) + 1
        for c in counts.values():
            self.assertLess(abs(c - 10000) / 10000, 0.05)

    def test_validation(self):
        with self.assertRaises(ValueError):
            custom_alphabet("", 5)
        with self.assertRaises(ValueError):
            custom_alphabet("x" * 257, 5)
        with self.assertRaises(ValueError):
            nanoid(0)


if __name__ == "__main__":
    unittest.main()
