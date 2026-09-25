"""Nano ID: compact, URL-friendly random string IDs.

Reference: https://github.com/ai/nanoid. Uses rejection sampling with a
bit mask so every symbol of a custom alphabet is equally likely.
"""

from __future__ import annotations

import os
from typing import Callable

__all__ = ["URL_ALPHABET", "DEFAULT_SIZE", "nanoid", "custom_alphabet", "custom_random"]

URL_ALPHABET = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict"
DEFAULT_SIZE = 21

RandomSource = Callable[[int], bytes]

_URL_TABLE = bytes(URL_ALPHABET.encode("ascii")[b & 63] for b in range(256))


def nanoid(size: int = DEFAULT_SIZE) -> str:
    """Return a random ID using the URL-safe 64-symbol alphabet."""
    if size < 1:
        raise ValueError("size must be >= 1")
    return os.urandom(size).translate(_URL_TABLE).decode("ascii")


def _mask_for(length: int) -> int:
    return (2 << (((length - 1) | 1).bit_length() - 1)) - 1


def _step_for(mask: int, length: int, size: int) -> int:
    if mask + 1 == length:
        return size
    return -(-8 * mask * size // (5 * length))


class _Generator:
    __slots__ = ("alphabet", "size", "_random", "_mask", "_length", "_table", "_delete")

    def __init__(self, alphabet: str, size: int, random: RandomSource) -> None:
        if not 1 <= len(alphabet) <= 256:
            raise ValueError("alphabet must contain between 1 and 256 symbols")
        if size < 1:
            raise ValueError("size must be >= 1")
        self.alphabet = alphabet
        self.size = size
        self._random = random
        self._length = len(alphabet)
        self._mask = _mask_for(self._length)
        self._table = None
        self._delete = b""
        if alphabet.isascii():
            symbols = alphabet.encode("ascii")
            table = bytearray(256)
            delete = bytearray()
            for b in range(256):
                index = b & self._mask
                if index < self._length:
                    table[b] = symbols[index]
                else:
                    delete.append(b)
            self._table = bytes(table)
            self._delete = bytes(delete)

    def __call__(self, size: int | None = None) -> str:
        size = self.size if size is None else size
        if size < 1:
            raise ValueError("size must be >= 1")
        step = _step_for(self._mask, self._length, size)
        if self._table is not None:
            out = b""
            while True:
                out += self._random(step).translate(self._table, self._delete)
                if len(out) >= size:
                    return out[:size].decode("ascii")
        chars: list[str] = []
        while True:
            for b in self._random(step):
                index = b & self._mask
                if index < self._length:
                    chars.append(self.alphabet[index])
                    if len(chars) == size:
                        return "".join(chars)


def custom_alphabet(alphabet: str, size: int = DEFAULT_SIZE) -> Callable[..., str]:
    """Return a generator ``fn(size=None) -> str`` bound to ``alphabet``."""
    return _Generator(alphabet, size, os.urandom)


def custom_random(alphabet: str, size: int, random: RandomSource) -> Callable[..., str]:
    """Like :func:`custom_alphabet` but with a caller-supplied byte source."""
    return _Generator(alphabet, size, random)
