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

## UUIDv4 and UUIDv7

Both follow [RFC 9562](https://www.rfc-editor.org/rfc/rfc9562).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    48-bit time (ms, Unix)                     |   v7 only;
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+   random in v4
|                               |  ver  |       rand_a          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|var|                        rand_b                             |
+-+-+                                                           +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Binary form:** 16 bytes, big-endian, the same bytes as the text form.
- **Version and variant:** after filling the bytes, set
  `b[6] = (b[6] & 0x0F) | (version << 4)` and `b[8] = (b[8] & 0x3F) | 0x80`.
- **UUIDv4:** 16 bytes from the OS CSPRNG, then version 4. 122 random bits.
- **UUIDv7:** bytes 0–5 are the timestamp and bytes 6–15 are 10 bytes from the
  OS CSPRNG, then version 7. 74 random bits: the 12-bit `rand_a` followed by the
  62-bit `rand_b`. The timestamp must be in 0 to 2⁴⁸−1 ms; anything outside is
  an error. Where the platform's standard library already generates UUIDs
  (Python's `uuid.uuid4()` and, from 3.14, `uuid.uuid7()`; Java's
  `UUID.randomUUID()`), the library uses it.
- **Text form:** 36 characters, lowercase hex in 8-4-4-4-12 groups separated by
  `-`, for example `017f22e2-79b0-7cc3-98c4-dc0c0c07398f`.
- **Parsing:**
  - Case-insensitive.
  - The length must be exactly 36, with `-` at offsets 8, 13, 18 and 23 and a
    hex digit everywhere else. Braces, `urn:uuid:` prefixes and the 32-digit
    form without hyphens are rejected, so every UUID has exactly one text form.
  - Any version and variant is accepted, including the nil and max UUIDs. The
    version is `b[6] >> 4`; the timestamp is only defined for version 7.
- **Monotonic UUIDv7** (RFC 9562 §6.2, monotonic random). The generator keeps
  `(last_ms, last_rand)`, where `last_rand` is the 74 random bits read as one
  integer `rand_a << 62 | rand_b`. On each call:
  - If `now > last_ms`, draw fresh randomness and store `(now, rand)`.
  - Otherwise (the same millisecond, or the clock went backwards), set
    `last_rand += 1` as a 74-bit integer and keep `last_ms`. The version and
    variant bits are not part of the counter.
  - If the increment would pass 2⁷⁴−1, return an overflow error and leave the
    state unchanged.

## Relative ID

A relative ID is sortable and unique, and every ID generated for the same
caller-supplied key (a customer, tenant or device id) starts with the same tag:

```
3F8KQZ-01J8Y4Q9M3-X3B0N6E3P8
|----| |--------| |--------|
 tag    48-bit ms  50-bit random / counter
```

- **Value:** a 128-bit unsigned integer `tag << 98 | ms << 50 | rand`, with a
  30-bit tag, a 48-bit Unix time in ms and 50 random bits. Sorting by value
  (or by text) sorts by tag, then time, then `rand`.
- **Tag:** `tag = BE32(HMAC-SHA-256(secret, input)[0..4]) >> 2`, the top 30 bits
  of the MAC, where `input = BE32(len(salt)) ‖ salt ‖ key`. `salt` and `key`
  are UTF-8 bytes and `BE32` is a 4-byte big-endian integer. The length prefix
  keeps salt `ab` with key `c` apart from salt `a` with key `bc`.
  - The **secret** must be at least 16 bytes. It is what stops anyone from
    computing the tag of a guessed key; it must come from configuration or a
    key store, never from source code.
  - The **salt** is optional (empty by default) and not secret. Generators
    with different salts give the same key unrelated tags, which separates
    contexts (for example `orders` and `invoices`) under one secret.
  - Two keys share a tag with probability 2⁻³⁰; there is a 50% chance that
    some pair among about 38,600 keys does. A shared tag only mixes those keys'
    IDs in a prefix scan; it never makes two IDs equal.
- **Text form:** 28 characters, three fields of Crockford base32 (the ULID
  alphabet) joined by `-`: the tag in 6 characters, the time in 10 and `rand`
  in 10, each field most significant symbol first. The time field has 2
  leading zero bits, so its first character is `0`–`7`. Every ID with a given
  tag starts with the tag's 6 characters followed by `-`, so
  `id >= 'TAG6CH-' AND id < 'TAG6CH.'` (or `LIKE 'TAG6CH-%'`) selects them.
- **Decoding:**
  - Case-insensitive, with the same character rules as ULID (no aliases).
  - Either the 28-character form with `-` at offsets 6 and 17, or the same 26
    characters without hyphens. Anything else is rejected.
  - A first time character above `7` is rejected as an overflow.
- **Randomness:** `rand` is 7 bytes from the OS CSPRNG, read big-endian and
  masked to 50 bits.
- **Timestamp range:** 0 to 2⁴⁸−1 ms, as for ULID.
- **Monotonic generation:** one `(last_ms, last_rand)` state per generator,
  shared by every key, following the ULID rules with a 50-bit counter:
  - If `now > last_ms`, draw fresh randomness and store `(now, rand)`.
  - Otherwise, set `last_rand += 1` and keep `last_ms`.
  - If the increment would pass 2⁵⁰−1, return an overflow error and leave the
    state unchanged.

  Because `(ms, rand)` never repeats within a generator, its IDs are unique
  even when two keys share a tag, and each key's IDs strictly increase.
  Consecutive `rand` values do reveal that two IDs came from the same
  generator in the same millisecond.
- **Not a secret token:** 50 random bits are enough to avoid collisions
  between generators (a 50% chance needs about 40 million IDs for one tag in
  one millisecond), not to resist guessing.

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
