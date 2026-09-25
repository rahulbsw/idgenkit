# Algorithms

This is the contract every implementation in this repository follows.
`testdata/` holds the conformance vectors that enforce it. Regenerate them with
`python3 testdata/generate_vectors.py`; the invalid-ULID list is maintained by hand.

## ULID

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      48-bit time (ms, Unix)                   |
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               |       80-bit randomness       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Binary form:** 16 bytes, big-endian. The time takes bytes 0–5 and the
  randomness bytes 6–15, so byte order equals time order. The same 16 bytes are
  used for the UUID mapping (`ulid_to_uuid`, `toUuid`, and so on). No version
  bits are forced.
- **Text form:** 26 characters of Crockford base32,
  `0123456789ABCDEFGHJKMNPQRSTVWXYZ`. The 128-bit value is left-padded with 2
  zero bits to 130 bits and encoded 5 bits at a time, most significant first.
  The first character therefore encodes only 3 bits and must be `0`–`7`.
- **Decoding:**
  - Case-insensitive.
  - The length must be exactly 26.
  - `I`, `L`, `O`, `U` and every character outside the alphabet are rejected.
    The spec's optional aliasing of `I/L→1` and `O→0` is deliberately not
    applied, so every ID has exactly one text form.
  - A first character above `7` is rejected as an overflow.
- **Timestamp range:** 0 to 2⁴⁸−1 ms (the year 10889). Anything outside is an error.
- **Randomness:** 10 bytes from the OS CSPRNG for every new ID.
- **Monotonic generation:** the generator keeps `(last_ms, last_rand)`. On each call:
  - If `now > last_ms`, draw fresh randomness and store `(now, rand)`.
  - Otherwise (the same millisecond, or the clock went backwards), set
    `last_rand += 1` as an 80-bit integer and keep `last_ms`. The result is
    therefore strictly increasing even across clock regressions.
  - If the increment would pass 2⁸⁰−1, return an overflow error and leave the
    state unchanged.

## Snowflake

```
 63                                        22 21        12 11          0
+--------------------------------------------+------------+-------------+
|   42-bit ms since epoch                    | 10-bit mid | 12-bit seq  |
+--------------------------------------------+------------+-------------+
```

- The epoch defaults to 0 (the Unix epoch) and the machine id defaults to 1,
  as in `dustinrouillard/snowflake-id`. The machine id must be in 0–1023.
- **Generator state:** one 64-bit word `(last_ms << 12) | seq`, updated with a
  compare-and-swap (or under a mutex in Python), so concurrent callers never
  receive the same ID. On each call:
  - If `now > last_ms`, the sequence restarts at 0 with `now`.
  - Otherwise (the same millisecond, or the clock went backwards), `seq += 1`
    and `last_ms` is kept.
  - If `seq` would pass 4095, spin until the clock passes `last_ms`. This caps
    output at 4,096 IDs per millisecond per generator.
- An error is returned when `ms − epoch` doesn't fit in 42 bits, or is negative.
- **Signed range:** the IDs are unsigned 64-bit values. Hosts with only signed
  64-bit integers (Java, PostgreSQL, MySQL) raise an error when bit 63 would be
  set, which happens in 2039 with epoch 0. Use a custom epoch to avoid it.
- **Parsing:**
  - `timestamp = (id >> 22) + epoch`
  - `machine_id = (id >> 12) & 0x3FF`
  - `sequence = id & 0xFFF`

## Nano ID

- The default alphabet is
  `useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict` (64
  symbols) and the default size is 21.
- Custom alphabets must have 1–256 symbols. The size must be at least 1.
  The database extensions cap it at 1024.
- **Default alphabet:** each output symbol is `alphabet[byte & 63]`. This is
  unbiased because 64 divides 256.
- **Custom alphabet**, with `n` symbols (the reference's `customRandom`):
  1. `mask = (2 << floor(log2((n − 1) | 1))) − 1`, the smallest `2^k − 1 ≥ n − 1`.
  2. If `mask + 1 == n`, then `step = size`. Otherwise
     `step = ceil(8 · mask · size / (5 · n))`, which is the reference's
     `ceil(1.6 · mask · size / n)` computed exactly in integers.
  3. Repeat: draw `step` random bytes; for each byte in order,
     `i = byte & mask`; if `i < n`, append `alphabet[i]`; stop once `size`
     symbols have been collected.
- Rejection sampling keeps the distribution uniform. Consuming the bytes in
  forward order makes the output a pure function of the random stream, which is
  what `testdata/nanoid.txt` checks.
