# Compared with UUID

UUIDs are the default unique identifier for good reasons, and this page doesn't
try to talk you out of them. It sets out what each format actually does, where
UUIDv4 and UUIDv7 are the better choice, and where ULID, Snowflake or Nano ID
give you something UUID doesn't. Storage figures come from the
[measurements on the data engineering page](data-engineering.md).

## Short answer

- **If you already have UUID columns and want time ordering, UUIDv7 is usually
  the simplest choice.** It's an IETF standard (RFC 9562, 2024), fits every
  native `uuid` type, and gives the same index benefits as ULID. PostgreSQL 18
  has it built in as `uuidv7()`, as does Python 3.14 as `uuid.uuid7()`.
  idgenkit provides it everywhere else: PostgreSQL 14–17, MySQL, Redis, Go,
  Rust, Java and older Python, plus a monotonic mode like ULID's.
- **If you need identifiers that reveal nothing, UUIDv4 or Nano ID are the
  right choice.** Any time-ordered format leaks when the record was created.
- **ULID** earns its place when the ID is handled as text: 26 case-insensitive
  characters instead of 36, no hyphens, and a specified monotonic mode. In
  binary it is the same 16 bytes as a UUID and can live in a `uuid` column.
- **Snowflake** earns its place when size and speed matter most and you
  control every writer: 8 bytes instead of 16, a native `BIGINT` everywhere,
  and columns that compress almost like a sequence. It also brings the most
  operational constraints of any format here.
- **Nano ID** earns its place for short, public, URL-safe tokens, with a
  configurable length and alphabet.

## The formats side by side

| | UUIDv4 | UUIDv7 | ULID | Snowflake | Nano ID |
|---|---|---|---|---|---|
| Specification | RFC 9562 (IETF) | RFC 9562 (IETF, 2024) | community spec | de facto (Twitter, 2010); layouts vary | community spec |
| Size | 128 bits | 128 bits | 128 bits | 64 bits | 21 characters (126 bits) |
| Random bits | 122 | 74 (some implementations use 12 of them as a counter) | 80 | 0 | 126 |
| Time component | none | 48-bit Unix ms | 48-bit Unix ms | 42-bit ms since a chosen epoch | none |
| Sorts by creation time | no | yes, to the ms; within a ms depends on the implementation | yes, to the ms; within a ms only in monotonic mode | yes, to the ms, then sequence, per generator | no |
| Binary size | 16 bytes | 16 bytes | 16 bytes | 8 bytes | no standard binary form |
| Text form | 36 hex characters with hyphens | 36 | 26 Crockford base32, case-insensitive | up to 19 decimal digits | 21 URL-safe characters |
| Coordination between generators | none | none | none | unique machine id per generator (1,024 max) | none |
| Throughput per generator | unbounded | unbounded | unbounded | 4,096 per ms, then waits | unbounded |
| Native database type | `uuid` in PostgreSQL, ClickHouse and others; `uniqueidentifier` in SQL Server; MySQL uses `BINARY(16)` | same as v4 | none: store as `uuid`, `BINARY(16)` or text | `BIGINT` everywhere | text |
| Can be guessed | no | timestamp visible; random part isn't guessable | timestamp visible; in monotonic mode, the next ID in the same ms is the previous one plus one | yes, from the time and machine id | no |

## Where UUID is the better choice

**Ecosystem support.** UUID is the format every tool already understands:
native types in most databases, JSON Schema and OpenAPI `format: uuid`, ORM
column types, Parquet's `UUID` logical type, Iceberg's `uuid` type, Avro's
`uuid` logical type and Arrow's canonical UUID extension type. ULID and Nano ID
have none of these. A ULID can be stored in a `uuid` column, but tools that
check the version field will see an invalid UUID version, and anyone reading the
column sees hex, not the ULID text form.

**UUIDv7 closes most of the gap with ULID.** It gives the same 48-bit
millisecond prefix, the same index locality and the same compression behaviour.
In our PostgreSQL 18 measurements, `uuidv7()` and monotonic ULID stored as `uuid`
produced byte-for-byte the same index size and page density. If you don't need
ULID's text form, UUIDv7 gives you ULID's ordering without leaving the standard.

**UUIDv4 reveals nothing.** It has no timestamp, no machine id and no ordering.
For public identifiers, security tokens, or data where creation time is
sensitive, that's a feature, not a flaw.

**Random keys spread writes in range-sharded databases.** Distributed databases
that split tables by key range (Spanner, CockroachDB, TiDB, HBase and Bigtable
row keys) send every new time-ordered key to the same range, creating a write
hotspot. Their documentation recommends random keys such as UUIDv4, or hashing a
prefix. Hash-partitioned systems (Cassandra, DynamoDB, Kafka keys) aren't
affected.

**No clock, no coordination.** UUIDv4 needs only a random number generator. Any
time-based ID depends on a working clock, and Snowflake also depends on every
generator having a unique machine id. A misconfigured machine id means
duplicate keys.

## Where ULID, Snowflake and Nano ID are the better choice

### Snowflake: half the bytes

A Snowflake ID is a plain 64-bit integer. In PostgreSQL its primary-key index
costs 22.5 bytes per row, the same as a `bigint` sequence. A UUIDv7 index costs
31.5 and a UUIDv4 index costs 40.5. Joins, sorts and hash tables work on 8-byte
keys, and in our compression test a column of Snowflake IDs shrinks to about 2
bytes per value with zlib and under half a byte with LZMA, because consecutive
IDs differ only in their low bits.

The costs are real and should be planned for:
- Each generator needs a **unique machine id** (0–1023). Assign them from
  configuration or a registry, never at random.
- Each generator is limited to **4,096 IDs per millisecond** and then waits for
  the next millisecond. To go faster, add generators.
- IDs exceed JavaScript's safe integer range (2⁵³), so **send them as strings in
  JSON**, as Twitter and Discord do.
- With epoch 0, IDs no longer fit a signed 64-bit integer from 2039. Pick a
  **custom epoch**; `1288834974657` (Twitter's) lasts until about 2080.
- IDs reveal creation time and machine id and are **easy to enumerate**. Don't
  expose them where that matters.

### ULID: UUID-sized, shorter text

Where the ID is handled as a string (URLs, file names, object-storage keys, log
lines, Kafka keys, CSV exports), ULID's 26 characters are 28% shorter than a
UUID's 36. They're case-insensitive, and there's no punctuation to strip or
double-click around. Text ULIDs sort in time order. Canonical lowercase UUIDv7
text does too, so that alone isn't a reason to choose ULID.

ULID also specifies a **monotonic mode**: within a millisecond, each ID is the
previous one plus one, so a generator's IDs are strictly increasing. That
matters at high rates. Our measurements show that plain (non-monotonic) ULIDs
bulk-inserted into PostgreSQL fill their index only as well as random UUIDv4,
because thousands of IDs share each millisecond in random order. RFC 9562 makes
UUIDv7's sub-millisecond counter optional, so the same applies to UUIDv7
libraries that leave it out. PostgreSQL 18's `uuidv7()` includes one.

In binary a ULID is 16 bytes, just like a UUID, so `ulid_to_uuid()` stores it in
a native `uuid` column at no extra cost.

### Nano ID: short random tokens

Nano ID's default 21 characters carry 126 random bits, slightly more than
UUIDv4's 122, in a URL-safe form that is 15 characters shorter. Length and
alphabet are configurable, which suits invite codes, share links and short URLs.
Check the collision probability for your length before shortening it. As a
primary key, Nano ID behaves like UUIDv4 stored as text: random index inserts
and no compression.

### One implementation everywhere

A practical advantage rather than a format one: the same generator semantics are
available in Java, Rust, Go, Python, PostgreSQL, MySQL and Redis, and all of
them are tested against shared vectors. With UUIDv7, behaviour inside a
millisecond (counter or no counter, clock rollback handling) varies between
libraries.

## Decision guide

| Situation | Suggested format | Why |
|---|---|---|
| Existing `uuid` columns; you want better index locality | UUIDv7, or ULID as `uuid` via the monotonic generator | Same type and size, sequential inserts |
| Highest write throughput, smallest storage, you run every writer | Snowflake | 8-byte keys, native `BIGINT`, compresses like a sequence |
| Public identifiers, share links, invite codes | Nano ID or UUIDv4 | No timestamp, not guessable |
| Range-sharded distributed database | UUIDv4 or Nano ID, or a hashed prefix | Avoids a hotspot on the newest range |
| IDs created offline or in clients, merged later | UUIDv7, ULID, UUIDv4 or Nano ID | No machine-id coordination needed |
| IDs that live in URLs, file names or object-store keys | ULID | 26 case-insensitive characters, sorted by time |
| Interop with systems that have a native UUID type | UUIDv7 or UUIDv4 | Standard type, no conversion |
| Records whose creation time is sensitive | UUIDv4 or Nano ID | Time-ordered IDs reveal it |

## Things to check in your own stack

- **Native UUID types don't all sort by byte order.** SQL Server's
  `uniqueidentifier`, for example, compares the last six bytes first, so
  time-ordered UUIDs and ULIDs stored in it don't sort by time. Check how your
  engine orders its UUID type before relying on it.
- **Text collation.** Store ULID and Nano ID text with a byte-wise collation
  (`COLLATE "C"` in PostgreSQL, `ascii_bin` in MySQL). Locale-aware collations
  are slower and can reorder characters.
- **Never store UUIDs as text if you can avoid it.** In our measurements, UUID
  text costs nearly twice the index space of the `uuid` type.
