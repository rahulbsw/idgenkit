"""Snowflake: time-ordered 64-bit IDs.

Layout (compatible with https://github.com/dustinrouillard/snowflake-id):

    | 42 bits: ms since epoch | 10 bits: machine id | 12 bits: sequence |
"""

from __future__ import annotations

import threading
import time
from typing import NamedTuple

__all__ = ["Snowflake", "SnowflakeParts", "compose", "parse"]

TIMESTAMP_BITS = 42
MACHINE_ID_BITS = 10
SEQUENCE_BITS = 12

MAX_MACHINE_ID = (1 << MACHINE_ID_BITS) - 1
MAX_SEQUENCE = (1 << SEQUENCE_BITS) - 1
MAX_TIMESTAMP_DELTA = (1 << TIMESTAMP_BITS) - 1

_MACHINE_SHIFT = SEQUENCE_BITS
_TIMESTAMP_SHIFT = SEQUENCE_BITS + MACHINE_ID_BITS


class SnowflakeParts(NamedTuple):
    timestamp_ms: int
    machine_id: int
    sequence: int


def compose(timestamp_ms: int, machine_id: int, sequence: int, epoch_ms: int = 0) -> int:
    delta = timestamp_ms - epoch_ms
    if not 0 <= delta <= MAX_TIMESTAMP_DELTA:
        raise ValueError("timestamp is outside the 42-bit range for this epoch")
    if not 0 <= machine_id <= MAX_MACHINE_ID:
        raise ValueError(f"machine_id must be in [0, {MAX_MACHINE_ID}]")
    if not 0 <= sequence <= MAX_SEQUENCE:
        raise ValueError(f"sequence must be in [0, {MAX_SEQUENCE}]")
    return (delta << _TIMESTAMP_SHIFT) | (machine_id << _MACHINE_SHIFT) | sequence


def parse(snowflake_id: int, epoch_ms: int = 0) -> SnowflakeParts:
    if not 0 <= snowflake_id < (1 << 64):
        raise ValueError("snowflake id must be an unsigned 64-bit integer")
    return SnowflakeParts(
        timestamp_ms=(snowflake_id >> _TIMESTAMP_SHIFT) + epoch_ms,
        machine_id=(snowflake_id >> _MACHINE_SHIFT) & MAX_MACHINE_ID,
        sequence=snowflake_id & MAX_SEQUENCE,
    )


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


class Snowflake:
    """Thread-safe Snowflake generator.

    When 4096 IDs have been issued in one millisecond the generator waits for
    the next millisecond. If the wall clock moves backwards the generator keeps
    issuing from the last observed millisecond, so IDs never go backwards.
    """

    __slots__ = ("machine_id", "epoch_ms", "_lock", "_last_ms", "_sequence")

    def __init__(self, machine_id: int = 1, epoch_ms: int = 0) -> None:
        if not 0 <= machine_id <= MAX_MACHINE_ID:
            raise ValueError(f"machine_id must be in [0, {MAX_MACHINE_ID}]")
        if epoch_ms < 0:
            raise ValueError("epoch_ms must be non-negative")
        self.machine_id = machine_id
        self.epoch_ms = epoch_ms
        self._lock = threading.Lock()
        self._last_ms = -1
        self._sequence = 0

    def next_id(self) -> int:
        with self._lock:
            now = _now_ms()
            if now > self._last_ms:
                self._last_ms = now
                self._sequence = 0
            elif self._sequence < MAX_SEQUENCE:
                self._sequence += 1
            else:
                while now <= self._last_ms:
                    now = _now_ms()
                self._last_ms = now
                self._sequence = 0
            delta = self._last_ms - self.epoch_ms
            if not 0 <= delta <= MAX_TIMESTAMP_DELTA:
                raise OverflowError("current time is outside the 42-bit range for this epoch")
            return (delta << _TIMESTAMP_SHIFT) | (self.machine_id << _MACHINE_SHIFT) | self._sequence

    def parse(self, snowflake_id: int) -> SnowflakeParts:
        return parse(snowflake_id, self.epoch_ms)
