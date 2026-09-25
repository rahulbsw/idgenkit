# idgenkit

Dependency-free generators for **ULID**, **Snowflake** and **Nano ID**, written
natively in Java, Rust, Go and Python, with extensions for PostgreSQL, MySQL and
Redis. Every implementation follows the same [specification](ALGORITHMS.md) and
is tested against the same conformance vectors, so an ID produced in Go parses
the same way in Java, in SQL or in a Redis command.

- **No third-party runtime dependencies.** Each library uses only its standard
  library and the operating system's CSPRNG.
- **One C core for the databases.** PostgreSQL, MySQL and Redis compile the
  same `c/idgenkit.c`, so the logic lives in one place.
- **Measured, not claimed.** [Storage](data-engineering.md) and
  [speed](benchmarks.md) numbers come from scripts in the repository that you
  can rerun.

## The three formats at a glance

| | ULID | Snowflake | Nano ID |
|---|---|---|---|
| Example | `01ARYZ6S41TSV4RRFFQ69G5FAV` | `1541815603606036480` | `V1StGXR8_Z5jdHi6B-myT` |
| Size | 128 bits (16 bytes) | 64 bits (8 bytes) | 126 random bits, 21 characters |
| Text form | 26 characters, Crockford base32 | decimal integer | 21 characters, URL-safe |
| Ordered by creation time | yes, to the millisecond | yes, to the millisecond | no |
| Coordination between generators | none | a unique machine id per generator | none |
| Typical use | UUID-sized keys with time order and a shorter text form | compact primary keys and event ids where you run the writers | public, unguessable tokens and short URLs |

## Which one should I use?

There's no single winner, and UUID is often the right answer. The
[comparison with UUID](comparison.md) goes through the trade-offs, including
where UUIDv4 and UUIDv7 are the better choice. In short:

- **You already use `uuid` columns and want better index locality:** use
  UUIDv7. It's built into PostgreSQL 18 as `uuidv7()`. ULID stored as `uuid` is
  equivalent in size and ordering if you use the monotonic generator.
- **You want the smallest, fastest keys and control every writer:** use
  Snowflake. It's half the size of any UUID and compresses like a sequence, but
  needs unique machine ids and must be sent as a string to JavaScript.
- **You want identifiers that reveal nothing and can't be guessed:** use Nano ID
  or UUIDv4. Never use Snowflake or a sequence for public identifiers.
- **Your database shards by key range (Spanner, CockroachDB, TiDB, HBase):**
  prefer random keys. Time-ordered keys concentrate new writes on one shard.

## Get it

| | Install | Import |
|---|---|---|
| Python ≥ 3.9 | `pip install idgenkit` | `import idgenkit` |
| Rust ≥ 1.74 | `cargo add idgenkit` | `use idgenkit::…` |
| Go ≥ 1.22 | `go get github.com/rahulbsw/idgenkit/go` | `github.com/rahulbsw/idgenkit/go/ulid` |
| Java ≥ 17 | `io.github.rahulbsw:idgenkit` | `io.github.rahulbsw.idgenkit.*` |
| PostgreSQL 14–18 | release tarball or `make install` | `CREATE EXTENSION idgenkit;` |
| MySQL 8+ | release tarball or `make` | `SOURCE install.sql` |
| Redis 7+ | release tarball or `make` | `loadmodule idgenkit.so` |

See [Getting started](getting-started.md) for usage in every language and
database.
