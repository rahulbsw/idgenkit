"""ULID: Universally Unique Lexicographically Sortable Identifier.

Spec: https://github.com/ulid/spec

    01AN4Z07BY      79KA1307SR9X4MV3
    |----------|    |----------------|
     Timestamp          Randomness
      48 bits             80 bits
"""

from __future__ import annotations

import base64
import os
import threading
import time
import uuid

__all__ = ["ULID", "MonotonicULID", "generate", "MAX_TIMESTAMP"]

CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
MAX_TIMESTAMP = (1 << 48) - 1
_RANDOM_BITS = 80
_RANDOM_MAX = (1 << _RANDOM_BITS) - 1
_MAX_INT = (1 << 128) - 1

# ULID text is Crockford base32 over a 130-bit field (2 leading zero bits).
# We let the C-implemented RFC 4648 base32 codec do the heavy lifting on a
# 160-bit (20-byte, 32-char) buffer and translate the alphabets.
_RFC4648 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
_CROCKFORD = CROCKFORD_ALPHABET.encode("ascii")
_TO_CROCKFORD = bytes.maketrans(_RFC4648, _CROCKFORD)
_FROM_CROCKFORD = bytes.maketrans(_CROCKFORD + _CROCKFORD.lower(), _RFC4648 * 2)
_VALID_CHARS = _CROCKFORD + _CROCKFORD.lower()
_ENCODE_PAD = 6  # 32 chars - 26 chars


def _encode(value: int) -> str:
    return (
        base64.b32encode(value.to_bytes(20, "big"))
        .translate(_TO_CROCKFORD)[_ENCODE_PAD:]
        .decode("ascii")
    )


def _decode(text: str) -> int:
    if len(text) != 26:
        raise ValueError(f"ULID must be 26 characters, got {len(text)}")
    raw = text.encode("ascii")
    if raw.translate(None, _VALID_CHARS):
        raise ValueError(f"invalid ULID character in {text!r}")
    if raw[0] > ord("7"):
        raise ValueError(f"ULID overflows 128 bits: {text!r}")
    return int.from_bytes(base64.b32decode(b"AAAAAA" + raw.translate(_FROM_CROCKFORD)), "big")


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
        now = _now_ms()
        with self._lock:
            if now <= self._last_ms:
                if self._last_rand == _RANDOM_MAX:
                    raise OverflowError("ULID random component overflow within one millisecond")
                self._last_rand += 1
                now = self._last_ms
            else:
                self._last_ms = now
                self._last_rand = int.from_bytes(os.urandom(10), "big")
            return ULID((now << _RANDOM_BITS) | self._last_rand)


def generate() -> str:
    """Return a new random ULID as its canonical 26-character string."""
    return _encode((_now_ms() << _RANDOM_BITS) | int.from_bytes(os.urandom(10), "big"))
