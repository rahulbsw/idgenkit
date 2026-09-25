# Getting started

Each library has the same three parts: ULID (with a monotonic generator),
Snowflake (with compose and parse), and Nano ID (with custom alphabets).

## Python

```sh
pip install idgenkit
```

```python
from idgenkit import ULID, MonotonicULID, Snowflake, nanoid, custom_alphabet

id = ULID.generate()            # str(id) -> '01J...', id.timestamp_ms, id.to_uuid()
parsed = ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
mono = MonotonicULID()          # thread-safe, strictly increasing
nxt = mono.next()

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

let id = Ulid::new();                       // Display -> 26 chars
let parsed = Ulid::parse("01ARYZ6S41TSV4RRFFQ69G5FAV")?;
let mono = MonotonicGenerator::new();       // Sync, strictly increasing
let next = mono.next()?;

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
)

id := ulid.New()                    // ulid.ULID ([16]byte), id.String() -> 26 chars
u, err := ulid.Parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
mono := ulid.NewMonotonic()         // safe for concurrent use
next, err := mono.Next()

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

Ulid id = Ulid.generate();                  // id.toString(), id.toUuid(), id.timestamp()
Ulid parsed = Ulid.parse("01ARYZ6S41TSV4RRFFQ69G5FAV");
MonotonicUlid mono = new MonotonicUlid();   // thread-safe
Ulid next = mono.next();

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
