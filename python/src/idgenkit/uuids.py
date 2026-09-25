"""RFC 9562 version 4 and version 7 UUIDs as standard ``uuid.UUID`` values.

    017f22e2-79b0-7cc3-98c4-dc0c0c07398f
    |-----------| |      |
     48-bit ms    ver 7  variant; the other 74 bits are random

``uuid4()`` is the standard library's. ``uuid7()`` is the standard library's
on Python 3.14+ (which fills rand_a/rand_b with a counter) and this module's
own, fully random, form on older versions.
"""

from __future__ import annotations

import os
import re
import threading
import time
import uuid
from typing import Callable

__all__ = [
    "MAX_TIMESTAMP",
    "MonotonicUUID7",
    "parse",
    "uuid4",
    "uuid4_from_bytes",
    "uuid7",
    "uuid7_from_parts",
    "uuid7_timestamp",
]

MAX_TIMESTAMP = (1 << 48) - 1
_RAND_B_MAX = (1 << 62) - 1
_RAND_MAX = (1 << 74) - 1
_CANONICAL = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
_STDLIB_UUID7 = getattr(uuid, "uuid7", None)


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


def _with_version(value: int, version: int) -> uuid.UUID:
    value = (value & ~(0xF << 76)) | (version << 76)
    return uuid.UUID(int=(value & ~(0b11 << 62)) | (0b10 << 62))


def uuid4() -> uuid.UUID:
    """Random (version 4) UUID; the same as ``uuid.uuid4()``."""
    return uuid.uuid4()


def uuid4_from_bytes(random: bytes) -> uuid.UUID:
    """Set the version and variant bits of 16 random bytes."""
    if len(random) != 16:
        raise ValueError("random must be 16 bytes")
    return _with_version(int.from_bytes(random, "big"), 4)


def uuid7_from_parts(timestamp_ms: int, random: bytes) -> uuid.UUID:
    """Build a UUIDv7; the version and variant bits overwrite 6 of the random bits."""
    if not 0 <= timestamp_ms <= MAX_TIMESTAMP:
        raise ValueError("timestamp must fit in 48 bits")
    if len(random) != 10:
        raise ValueError("random must be 10 bytes")
    return _with_version(timestamp_ms << 80 | int.from_bytes(random, "big"), 7)


def uuid7() -> uuid.UUID:
    """Time-ordered (version 7) UUID for the current time."""
    if _STDLIB_UUID7 is not None:
        return _STDLIB_UUID7()
    return uuid7_from_parts(_now_ms(), os.urandom(10))


def uuid7_timestamp(value: uuid.UUID) -> int:
    """Unix timestamp in milliseconds of a version 7 UUID."""
    if value.version != 7:
        raise ValueError(f"not a version 7 UUID: {value}")
    return value.int >> 80


def parse(text: str) -> uuid.UUID:
    """Parse the case-insensitive 8-4-4-4-12 form only.

    ``uuid.UUID(text)`` also accepts braces, ``urn:uuid:`` and 32 bare hex
    digits; this rejects them so every idgenkit library agrees.
    """
    if not isinstance(text, str) or not _CANONICAL.fullmatch(text):
        raise ValueError(f"UUID must be 36 characters in 8-4-4-4-12 hex form: {text!r}")
    return uuid.UUID(text)


class MonotonicUUID7:
    """Thread-safe generator whose UUIDv7s strictly increase.

    Within the same millisecond (or if the clock moves backwards) the previous
    74 random bits are incremented by one. Raises OverflowError if they are
    exhausted within a single millisecond.
    """

    __slots__ = ("_lock", "_last_ms", "_last_rand")

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._last_ms = -1
        self._last_rand = 0

    def next(self) -> uuid.UUID:
        return self._next_at(_now_ms(), os.urandom)

    def _next_at(self, now: int, random: Callable[[int], bytes]) -> uuid.UUID:
        with self._lock:
            if now <= self._last_ms:
                if self._last_rand == _RAND_MAX:
                    raise OverflowError("UUIDv7 random component overflow within one millisecond")
                self._last_rand += 1
            else:
                if now > MAX_TIMESTAMP:
                    raise ValueError("timestamp must fit in 48 bits")
                u = uuid7_from_parts(now, random(10))
                self._last_ms = now
                self._last_rand = (u.int >> 64 & 0xFFF) << 62 | (u.int & _RAND_B_MAX)
            r = self._last_rand
            return uuid.UUID(int=self._last_ms << 80 | 7 << 76 | (r >> 62) << 64 | 0b10 << 62 | (r & _RAND_B_MAX))
