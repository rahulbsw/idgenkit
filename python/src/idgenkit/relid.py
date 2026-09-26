"""Relative IDs: sortable, unique IDs that share a tag for the same key.

    3F8KQZ-01J8Y4Q9M3-X3B0N6E3P8
    |----| |--------| |--------|
     tag    48-bit ms  50-bit random / counter

The tag is the top 30 bits of HMAC-SHA-256(secret, BE32(len(salt)) || salt || key).
The secret keeps tags unguessable; the optional salt separates contexts.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import threading
import time
from typing import Callable, NamedTuple

__all__ = [
    "MAX_RANDOM",
    "MAX_TAG",
    "MAX_TIMESTAMP",
    "MIN_SECRET_BYTES",
    "RelativeId",
    "RelativeIdParts",
    "encode_tag",
    "from_parts",
    "parse",
]

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
MAX_TAG = (1 << 30) - 1
MAX_TIMESTAMP = (1 << 48) - 1
MAX_RANDOM = (1 << 50) - 1
MIN_SECRET_BYTES = 16
_DECODE = {c: i for i, c in enumerate(ALPHABET)} | {c.lower(): i for i, c in enumerate(ALPHABET)}


class RelativeIdParts(NamedTuple):
    tag: int
    timestamp_ms: int
    random: int

    @property
    def tag_text(self) -> str:
        return encode_tag(self.tag)


# Every field is a multiple of 10 bits, so each is encoded two symbols at a time.
_PAIRS = [a + b for a in ALPHABET for b in ALPHABET]


def _text(tag: int, ms: int, r: int) -> str:
    p = _PAIRS
    return (
        p[tag >> 20] + p[tag >> 10 & 1023] + p[tag & 1023] + "-"
        + p[ms >> 40] + p[ms >> 30 & 1023] + p[ms >> 20 & 1023] + p[ms >> 10 & 1023] + p[ms & 1023] + "-"
        + p[r >> 40] + p[r >> 30 & 1023] + p[r >> 20 & 1023] + p[r >> 10 & 1023] + p[r & 1023]
    )


def _tag_text(tag: int) -> str:
    return _PAIRS[tag >> 20] + _PAIRS[tag >> 10 & 1023] + _PAIRS[tag & 1023]


def _decode_field(text: str) -> int:
    value = 0
    for c in text:
        v = _DECODE.get(c)
        if v is None:
            raise ValueError(f"invalid relative ID character {c!r}")
        value = value << 5 | v
    return value


def encode_tag(tag: int) -> str:
    """The 6-character text form of a tag; every ID with it starts with this and ``-``."""
    if not 0 <= tag <= MAX_TAG:
        raise ValueError("tag must fit in 30 bits")
    return _tag_text(tag)


def from_parts(tag: int, timestamp_ms: int, random: int) -> str:
    if not 0 <= tag <= MAX_TAG:
        raise ValueError("tag must fit in 30 bits")
    if not 0 <= timestamp_ms <= MAX_TIMESTAMP:
        raise ValueError("timestamp must fit in 48 bits")
    if not 0 <= random <= MAX_RANDOM:
        raise ValueError("random must fit in 50 bits")
    return _text(tag, timestamp_ms, random)


def parse(text: str) -> RelativeIdParts:
    """Parse the 28-character form, or the same 26 characters without hyphens."""
    if not isinstance(text, str):
        raise ValueError("relative ID must be a string")
    if len(text) == 28 and text[6] == "-" and text[17] == "-":
        tag, ms, rand = text[:6], text[7:17], text[18:]
    elif len(text) == 26:
        tag, ms, rand = text[:6], text[6:16], text[16:]
    else:
        raise ValueError(f"relative ID must be 28 characters (or 26 without hyphens): {text!r}")
    if _DECODE.get(ms[0], 8) > 7:
        raise ValueError(f"invalid relative ID timestamp: {text!r}")
    return RelativeIdParts(_decode_field(tag), _decode_field(ms), _decode_field(rand))


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


class RelativeId:
    """Generates relative IDs for one secret and salt. Thread-safe.

    ``generate`` draws 50 fresh random bits per ID. ``monotonic`` shares one
    counter across all keys, so each key's IDs strictly increase and no two IDs
    from this generator are equal; it raises OverflowError if the counter is
    exhausted within a single millisecond.
    """

    __slots__ = ("_mac", "_lock", "_last_ms", "_last_rand")

    def __init__(self, secret: bytes, salt: str = "") -> None:
        if not isinstance(secret, (bytes, bytearray)) or len(secret) < MIN_SECRET_BYTES:
            raise ValueError(f"secret must be at least {MIN_SECRET_BYTES} bytes")
        salt_bytes = salt.encode("utf-8")
        if len(salt_bytes) > 0xFFFFFFFF:
            raise ValueError("salt is too long")
        self._mac = hmac.new(bytes(secret), len(salt_bytes).to_bytes(4, "big") + salt_bytes, hashlib.sha256)
        self._lock = threading.Lock()
        self._last_ms = -1
        self._last_rand = 0

    def tag_value(self, key: str) -> int:
        mac = self._mac.copy()
        mac.update(key.encode("utf-8"))
        return int.from_bytes(mac.digest()[:4], "big") >> 2

    def tag(self, key: str) -> str:
        """The 6-character tag that starts every ID for ``key``."""
        return _tag_text(self.tag_value(key))

    def generate(self, key: str) -> str:
        now = _now_ms()
        if now > MAX_TIMESTAMP:
            raise ValueError("timestamp must fit in 48 bits")
        return _text(self.tag_value(key), now, int.from_bytes(os.urandom(7), "big") & MAX_RANDOM)

    def monotonic(self, key: str) -> str:
        return self._monotonic_at(self.tag_value(key), _now_ms(), os.urandom)

    def _monotonic_at(self, tag: int, now: int, random: Callable[[int], bytes]) -> str:
        if not 0 <= tag <= MAX_TAG:
            raise ValueError("tag must fit in 30 bits")
        with self._lock:
            if now <= self._last_ms:
                if self._last_rand == MAX_RANDOM:
                    raise OverflowError("relative ID counter overflow within one millisecond")
                self._last_rand += 1
            else:
                if now > MAX_TIMESTAMP:
                    raise ValueError("timestamp must fit in 48 bits")
                self._last_ms = now
                self._last_rand = int.from_bytes(random(7), "big") & MAX_RANDOM
            return _text(tag, self._last_ms, self._last_rand)
