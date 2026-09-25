import os
import re
import sys
import threading
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))
TESTDATA = os.path.join(HERE, "..", "..", "testdata")

from idgenkit import (  # noqa: E402
    ULID,
    URL_ALPHABET,
    MonotonicULID,
    Snowflake,
    custom_alphabet,
    custom_random,
    nanoid,
)
from idgenkit import snowflake, ulid  # noqa: E402


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


class SnowflakeTest(unittest.TestCase):
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
