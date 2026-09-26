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

from .ulid import _PAIRS, _decode_base32

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

MAX_TAG = (1 << 30) - 1
MAX_TIMESTAMP = (1 << 48) - 1
MAX_RANDOM = (1 << 50) - 1
MIN_SECRET_BYTES = 16


class RelativeIdParts(NamedTuple):
    tag: int
    timestamp_ms: int
    random: int

    @property
    def tag_text(self) -> str:
        return encode_tag(self.tag)


# Every field is a multiple of 10 bits, so each is encoded two symbols at a time.
def _text(tag: int, ms: int, r: int) -> str:
    p = _PAIRS
    return (
        p[tag >> 20] + p[tag >> 10 & 1023] + p[tag & 1023] + "-"
        + p[ms >> 40] + p[ms >> 30 & 1023] + p[ms >> 20 & 1023] + p[ms >> 10 & 1023] + p[ms & 1023] + "-"
        + p[r >> 40] + p[r >> 30 & 1023] + p[r >> 20 & 1023] + p[r >> 10 & 1023] + p[r & 1023]
    )


# (ms, its text + "-"): consecutive IDs usually share a millisecond.
_ms_text = (-1, "")


def _suffix(ms: int, r: int) -> str:
    global _ms_text
    p = _PAIRS
    cached = _ms_text
    if cached[0] != ms:
        cached = _ms_text = (
            ms, f"{p[ms >> 40]}{p[ms >> 30 & 1023]}{p[ms >> 20 & 1023]}{p[ms >> 10 & 1023]}{p[ms & 1023]}-"
        )
    return f"{cached[1]}{p[r >> 40]}{p[r >> 30 & 1023]}{p[r >> 20 & 1023]}{p[r >> 10 & 1023]}{p[r & 1023]}"


def _tag_text(tag: int) -> str:
    return _PAIRS[tag >> 20] + _PAIRS[tag >> 10 & 1023] + _PAIRS[tag & 1023]


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
    parts = RelativeIdParts(_decode_base32(tag), _decode_base32(ms), _decode_base32(rand))
    if parts.timestamp_ms > MAX_TIMESTAMP:
        raise ValueError(f"invalid relative ID timestamp: {text!r}")
    return parts


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


# Keys up to _CACHE_MAX_KEY characters have their tag cached; the cache is
# emptied when it reaches _CACHE_SIZE keys.
_CACHE_SIZE = 1024
_CACHE_MAX_KEY = 64


class RelativeId:
    """Generates relative IDs for one secret and salt. Thread-safe.

    ``generate`` draws 50 fresh random bits per ID. ``monotonic`` shares one
    counter across all keys, so each key's IDs strictly increase and no two IDs
    from this generator are equal; it raises OverflowError if the counter is
    exhausted within a single millisecond.
    """

    __slots__ = ("_mac", "_lock", "_last_ms", "_last_rand", "_cache")

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
        self._cache: dict[str, tuple[int, str]] = {}

    def _compute_tag(self, key: str) -> int:
        mac = self._mac.copy()
        mac.update(key.encode("utf-8"))
        return int.from_bytes(mac.digest()[:4], "big") >> 2

    def _tag_entry(self, key: str) -> tuple[int, str]:
        """(tag, tag text + "-") for key."""
        entry = self._cache.get(key)
        if entry is None:
            tag = self._compute_tag(key)
            entry = (tag, _tag_text(tag) + "-")
            if len(key) <= _CACHE_MAX_KEY:
                if len(self._cache) >= _CACHE_SIZE:
                    self._cache.clear()
                self._cache[key] = entry
        return entry

    def tag_value(self, key: str) -> int:
        return self._tag_entry(key)[0]

    def tag(self, key: str) -> str:
        """The 6-character tag that starts every ID for ``key``."""
        return self._tag_entry(key)[1][:6]

    def generate(self, key: str) -> str:
        now = _now_ms()
        if now > MAX_TIMESTAMP:
            raise ValueError("timestamp must fit in 48 bits")
        prefix = (self._cache.get(key) or self._tag_entry(key))[1]
        return prefix + _suffix(now, int.from_bytes(os.urandom(7), "big") & MAX_RANDOM)

    def monotonic(self, key: str) -> str:
        prefix = (self._cache.get(key) or self._tag_entry(key))[1]
        return self._monotonic_text(prefix, _now_ms(), os.urandom)

    def _monotonic_at(self, tag: int, now: int, random: Callable[[int], bytes]) -> str:
        if not 0 <= tag <= MAX_TAG:
            raise ValueError("tag must fit in 30 bits")
        return self._monotonic_text(_tag_text(tag) + "-", now, random)

    def _monotonic_text(self, prefix: str, now: int, random: Callable[[int], bytes]) -> str:
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
            return prefix + _suffix(self._last_ms, self._last_rand)
