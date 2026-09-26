"""ULID: Universally Unique Lexicographically Sortable Identifier.

Spec: https://github.com/ulid/spec

    01AN4Z07BY      79KA1307SR9X4MV3
    |----------|    |----------------|
     Timestamp          Randomness
      48 bits             80 bits
"""

from __future__ import annotations

import os
import threading
import time
import uuid
from typing import Callable

__all__ = ["ULID", "MonotonicULID", "generate", "MAX_TIMESTAMP"]

CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
MAX_TIMESTAMP = (1 << 48) - 1
_RANDOM_BITS = 80
_RANDOM_MAX = (1 << _RANDOM_BITS) - 1
_MAX_INT = (1 << 128) - 1

# ULID text is Crockford base32 over a 130-bit field (2 leading zero bits).
# Encoding looks up 10 bits (two symbols) at a time. Decoding maps Crockford
# onto the digits int() accepts in base 32, after rejecting everything else
# (int() would also allow signs, underscores and whitespace).
_CROCKFORD = CROCKFORD_ALPHABET.encode("ascii")
_VALID_CHARS = _CROCKFORD + _CROCKFORD.lower()
_TO_BASE32_DIGITS = bytes.maketrans(_VALID_CHARS, b"0123456789abcdefghijklmnopqrstuv" * 2)
_PAIRS = [a + b for a in CROCKFORD_ALPHABET for b in CROCKFORD_ALPHABET]
_MASK_40 = (1 << 40) - 1


def _encode(value: int) -> str:
    p = _PAIRS
    t = value >> _RANDOM_BITS
    a = value >> 40 & _MASK_40
    b = value & _MASK_40
    return (
        f"{p[t >> 40]}{p[t >> 30 & 1023]}{p[t >> 20 & 1023]}{p[t >> 10 & 1023]}{p[t & 1023]}"
        f"{p[a >> 30]}{p[a >> 20 & 1023]}{p[a >> 10 & 1023]}{p[a & 1023]}"
        f"{p[b >> 30]}{p[b >> 20 & 1023]}{p[b >> 10 & 1023]}{p[b & 1023]}"
    )


def _decode_base32(text: str) -> int:
    """Crockford base32 (either case) to int; ValueError for any other character."""
    raw = text.encode("ascii", "replace")
    if not raw or raw.translate(None, _VALID_CHARS):
        raise ValueError(f"invalid Crockford base32 character in {text!r}")
    return int(raw.translate(_TO_BASE32_DIGITS), 32)


def _decode(text: str) -> int:
    if len(text) != 26:
        raise ValueError(f"ULID must be 26 characters, got {len(text)}")
    value = _decode_base32(text)
    if value > _MAX_INT:
        raise ValueError(f"ULID overflows 128 bits: {text!r}")
    return value


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


class ULID:
    """An immutable 128-bit ULID. Ordering matches lexicographic string order."""

    __slots__ = ("_value",)

    def __init__(self, value: int) -> None:
        if not 0 <= value <= _MAX_INT:
            raise ValueError("ULID value must fit in 128 bits")
        self._value = value

    @classmethod
    def generate(cls) -> ULID:
        """Create a ULID from the current time and 80 bits of OS randomness."""
        return cls((_now_ms() << _RANDOM_BITS) | int.from_bytes(os.urandom(10), "big"))

    @classmethod
    def from_parts(cls, timestamp_ms: int, randomness: int) -> ULID:
        if not 0 <= timestamp_ms <= MAX_TIMESTAMP:
            raise ValueError("timestamp must fit in 48 bits")
        if not 0 <= randomness <= _RANDOM_MAX:
            raise ValueError("randomness must fit in 80 bits")
        return cls((timestamp_ms << _RANDOM_BITS) | randomness)

    @classmethod
    def parse(cls, text: str) -> ULID:
        return cls(_decode(text))

    @classmethod
    def from_bytes(cls, data: bytes) -> ULID:
        if len(data) != 16:
            raise ValueError("ULID binary form must be 16 bytes")
        return cls(int.from_bytes(data, "big"))

    @classmethod
    def from_uuid(cls, value: uuid.UUID) -> ULID:
        return cls(value.int)

    @property
    def timestamp_ms(self) -> int:
        return self._value >> _RANDOM_BITS

    @property
    def randomness(self) -> int:
        return self._value & _RANDOM_MAX

    def to_bytes(self) -> bytes:
        return self._value.to_bytes(16, "big")

    def to_uuid(self) -> uuid.UUID:
        return uuid.UUID(int=self._value)

    def __int__(self) -> int:
        return self._value

    def __str__(self) -> str:
        return _encode(self._value)

    def __repr__(self) -> str:
        return f"ULID({_encode(self._value)!r})"

    def __hash__(self) -> int:
        return hash(self._value)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, ULID) and self._value == other._value

    def __lt__(self, other: ULID) -> bool:
        return self._value < other._value

    def __le__(self, other: ULID) -> bool:
        return self._value <= other._value

    def __gt__(self, other: ULID) -> bool:
        return self._value > other._value

    def __ge__(self, other: ULID) -> bool:
        return self._value >= other._value


class MonotonicULID:
    """Thread-safe generator whose ULIDs strictly increase.

    Within the same millisecond (or if the clock moves backwards) the previous
    randomness is incremented by one. Raises OverflowError if the 80-bit random
    component is exhausted within a single millisecond.
    """

    __slots__ = ("_lock", "_last_ms", "_last_rand")

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._last_ms = -1
        self._last_rand = 0

    def next(self) -> ULID:
        return self._next_at(_now_ms(), os.urandom)

    def _next_at(self, now: int, random: Callable[[int], bytes]) -> ULID:
        with self._lock:
            if now <= self._last_ms:
                if self._last_rand == _RANDOM_MAX:
                    raise OverflowError("ULID random component overflow within one millisecond")
                self._last_rand += 1
                now = self._last_ms
            else:
                if now > MAX_TIMESTAMP:
                    raise ValueError("timestamp must fit in 48 bits")
                self._last_ms = now
                self._last_rand = int.from_bytes(random(10), "big")
            return ULID((now << _RANDOM_BITS) | self._last_rand)


def generate() -> str:
    """Return a new random ULID as its canonical 26-character string."""
    return _encode((_now_ms() << _RANDOM_BITS) | int.from_bytes(os.urandom(10), "big"))
