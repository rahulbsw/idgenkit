# Getting started

Each library has the same five parts: ULID (with a monotonic generator),
UUIDv4 and UUIDv7 (with a monotonic v7 generator and a strict parser),
relative IDs (keyed by a secret, with a monotonic mode), Snowflake (with
compose and parse), and Nano ID (with custom alphabets). Java and Python return
their standard `UUID` types. The [README](https://github.com/rahulbsw/idgenkit#relative-ids)
shows the relative ID API for each language and database, and how each one
loads its secret.

## Python

```sh
pip install idgenkit
```

```python
from idgenkit import ULID, MonotonicULID, MonotonicUUID7, Snowflake, nanoid, custom_alphabet
from idgenkit import uuid4, uuid7, uuid7_timestamp, uuids

id = ULID.generate()            # str(id) -> '01J...', id.timestamp_ms, id.to_uuid()
parsed = ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
mono = MonotonicULID()          # thread-safe, strictly increasing
nxt = mono.next()

v7 = uuid7()                    # uuid.UUID; the stdlib's uuid.uuid7() on 3.14+
v4 = uuid4()                    # uuid.uuid4()
ms = uuid7_timestamp(v7)        # ValueError for other versions
u = uuids.parse("017f22e2-79b0-7cc3-98c4-dc0c0c07398f")  # rejects braces, urn:, no hyphens
nxt7 = MonotonicUUID7().next()

gen = Snowflake(machine_id=7, epoch_ms=1288834974657)
sf = gen.next_id()
parts = gen.parse(sf)           # SnowflakeParts(timestamp_ms, machine_id, sequence)

s = nanoid()                    # 21 URL-safe characters
hex_id = custom_alphabet("0123456789abcdef", 12)
h = hex_id()
```

## Rust

```toml
[dependencies]
idgenkit = "0.1"
```

```rust
use idgenkit::{nanoid, snowflake::Snowflake, ulid::{MonotonicGenerator, Ulid}};
use idgenkit::uuid::{MonotonicV7Generator, Uuid};

let id = Ulid::new();                       // Display -> 26 chars
let parsed = Ulid::parse("01ARYZ6S41TSV4RRFFQ69G5FAV")?;
let mono = MonotonicGenerator::new();       // Sync, strictly increasing
let next = mono.next()?;

let v7 = Uuid::new_v7()?;                   // Display -> 36 chars
let v4 = Uuid::new_v4();
let ms = v7.timestamp_ms()?;                // Error::NotUuidV7 for other versions
let next7 = MonotonicV7Generator::new().next()?;

let gen = Snowflake::new(7, 1288834974657)?;
let sf = gen.next_id()?;
let parts = gen.parse(sf);

let s = nanoid::nanoid();
let hex = nanoid::CustomAlphabet::new("0123456789abcdef", 12)?;
let s = hex.generate();
```

## Go

```sh
go get github.com/rahulbsw/idgenkit/go
```

```go
import (
    "github.com/rahulbsw/idgenkit/go/nanoid"
    "github.com/rahulbsw/idgenkit/go/snowflake"
    "github.com/rahulbsw/idgenkit/go/ulid"
    "github.com/rahulbsw/idgenkit/go/uuid"
)

id := ulid.New()                    // ulid.ULID ([16]byte), id.String() -> 26 chars
u, err := ulid.Parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
mono := ulid.NewMonotonic()         // safe for concurrent use
next, err := mono.Next()

v7 := uuid.NewV7()                  // uuid.UUID ([16]byte), v7.String() -> 36 chars
v4 := uuid.NewV4()
ms, err := v7.Time()                // uuid.ErrNotV7 for other versions
next7, err := uuid.NewMonotonic().Next()

gen, err := snowflake.New(7, 1288834974657) // machine id, epoch ms
sf, err := gen.Next()
parts := gen.Parse(sf)              // TimestampMs, MachineID, Sequence

s := nanoid.New()
hex, err := nanoid.CustomAlphabet("0123456789abcdef", 12)
s = hex.Generate()
```

## Java

```xml
<dependency>
  <groupId>io.github.rahulbsw</groupId>
  <artifactId>idgenkit</artifactId>
  <version>0.1.0</version>
</dependency>
```

```java
import io.github.rahulbsw.idgenkit.*;
import java.util.UUID;

Ulid id = Ulid.generate();                  // id.toString(), id.toUuid(), id.timestamp()
Ulid parsed = Ulid.parse("01ARYZ6S41TSV4RRFFQ69G5FAV");
MonotonicUlid mono = new MonotonicUlid();   // thread-safe
Ulid next = mono.next();

UUID v7 = Uuids.v7();                       // java.util.UUID
UUID v4 = Uuids.v4();                       // UUID.randomUUID()
long ms = Uuids.timestamp(v7);              // IllegalArgumentException for other versions
UUID u = Uuids.parse("017f22e2-79b0-7cc3-98c4-dc0c0c07398f"); // stricter than UUID.fromString
UUID next7 = new MonotonicUuidV7().next();

Snowflake gen = new Snowflake(7, 1288834974657L);
long sf = gen.nextId();
Snowflake.Parts parts = gen.parse(sf);

String s = NanoId.generate();
NanoId hex = NanoId.customAlphabet("0123456789abcdef", 12);
String h = hex.next();
```

## PostgreSQL

Install from the release tarball for your PostgreSQL major version: copy `lib/*`
to `$(pg_config --pkglibdir)` and `extension/*` to
`$(pg_config --sharedir)/extension`. Or build it with `make -C postgres install`.

```
# postgresql.conf
shared_preload_libraries = 'idgenkit'         # needed for Snowflake before PostgreSQL 17
idgenkit.machine_id = 7                       # 0..1023, unique per server
idgenkit.snowflake_epoch_ms = '1288834974657' # set once, never change
```

```sql
CREATE EXTENSION idgenkit;

-- ULID stored in the native 16-byte uuid type
CREATE TABLE orders (
  id uuid PRIMARY KEY DEFAULT ulid_to_uuid(ulid_generate_monotonic()),
  ...
);
SELECT ulid_from_uuid(id), ulid_timestamp(id) FROM orders;

-- UUIDv7 on PostgreSQL 14+ (18 also has a built-in uuidv7())
CREATE TABLE invoices (id uuid PRIMARY KEY DEFAULT uuidv7_generate_monotonic(), ...);
SELECT uuidv7_timestamp(id) FROM invoices;

-- Snowflake as bigint
CREATE TABLE events (id bigint PRIMARY KEY DEFAULT snowflake_generate(), ...);
SELECT * FROM events WHERE id >= snowflake_from_timestamp(now() - interval '1 hour');

-- Nano ID for public tokens
ALTER TABLE orders ADD COLUMN share_token text COLLATE "C" UNIQUE DEFAULT nanoid_generate();
```

Every function:

| Function | Returns |
|---|---|
| `ulid_generate()` | `text`, 26 characters |
| `ulid_generate_monotonic()` | `text`, strictly increasing within the session |
| `ulid_generate_uuid()` | `uuid`, a ULID in the native type |
| `ulid_to_uuid(text)` / `ulid_from_uuid(uuid)` | conversion |
| `ulid_timestamp(text or uuid)` | `timestamptz` |
| `uuidv7_generate()` | `uuid`, version 7 |
| `uuidv7_generate_monotonic()` | `uuid`, version 7, strictly increasing within the session |
| `uuidv7_timestamp(uuid)` | `timestamptz`; error for other versions |
| `gen_random_uuid()` (built in) | `uuid`, version 4 |
| `snowflake_generate()` | `bigint` |
| `snowflake_timestamp(bigint)` | `timestamptz` |
| `snowflake_machine_id(bigint)`, `snowflake_sequence(bigint)` | `int` |
| `snowflake_from_timestamp(timestamptz)` | smallest id at that time, for range scans |
| `nanoid_generate(size = 21, alphabet = URL alphabet)` | `text` |

## MySQL

```sh
cp idgenkit_udf.so "$(mysql_config --plugindir)"
mysql -u root < install.sql
```

```sql
CREATE TABLE orders (id BINARY(16) PRIMARY KEY, ...);
INSERT INTO orders (id, ...) VALUES (ulid_to_bin(ulid_generate_monotonic()), ...);
SELECT bin_to_ulid(id), ulid_timestamp(bin_to_ulid(id)) FROM orders;

CREATE TABLE invoices (id BINARY(16) PRIMARY KEY, ...);
INSERT INTO invoices (id, ...) VALUES (UUID_TO_BIN(uuidv7_generate_monotonic()), ...);
SELECT BIN_TO_UUID(id), uuidv7_timestamp(id) FROM invoices;   -- ms since 1970
SELECT uuidv4_generate();

SELECT snowflake_generate(7, 1288834974657);   -- machine id, epoch
SELECT nanoid_generate(12, '0123456789abcdef');
```

MySQL folds table-independent function calls inside aggregates, just as it does
for `UUID()`: `SELECT COUNT(DISTINCT ulid_generate())` returns 1. Values
generated per row, including `INSERT ... SELECT`, are unique.

## Redis

```sh
redis-server --loadmodule /path/idgenkit.so MACHINE_ID 7 EPOCH_MS 1288834974657
```

```
> ULID.GENERATE
"01J8Y4Q9M3X3B0N6E3P8V7T2QK"
> ULID.MONOTONIC
> ULID.TIME 01ARYZ6S41TSV4RRFFQ69G5FAV
(integer) 1469918176385
> UUIDV7.GENERATE
> UUIDV7.MONOTONIC
> UUIDV7.TIME 017f22e2-79b0-7cc3-98c4-dc0c0c07398f
(integer) 1645557742000
> UUIDV4.GENERATE
> SNOWFLAKE.GENERATE
> SNOWFLAKE.PARSE 1541815603606036480
> NANOID.GENERATE 12 0123456789abcdef
```

## Build from source

```sh
git clone https://github.com/rahulbsw/idgenkit && cd idgenkit
make test        # every library and database extension
make bench       # benchmark report in bench/results/
```
