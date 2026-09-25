# idgenkit

Dependency-free generators for three unique-ID schemes, implemented natively in
Java, Rust, Go and Python, with database extensions for PostgreSQL, MySQL and Redis.

| Scheme | Size | Sortable | Spec |
|---|---|---|---|
| ULID | 128 bit, 26 chars Crockford base32 | by time (ms) | [ulid/spec](https://github.com/ulid/spec) |
| Snowflake | 64 bit integer | by time (ms) | [dustinrouillard/snowflake-id](https://github.com/dustinrouillard/snowflake-id) |
| Nano ID | 21 chars URL-safe (configurable) | no | [ai/nanoid](https://github.com/ai/nanoid) |

No third-party runtime or test dependencies anywhere: every implementation uses
only its standard library plus the operating system's CSPRNG. The database
extensions share a single C99 core (`c/`). All implementations are checked
against the same conformance vectors in `testdata/`.

Documentation: <http://github.datasierra.com/idgenkit/>, with sources in [`docs/`](docs/):

- [Compared with UUID](docs/comparison.md): when UUIDv4 or UUIDv7 is the better choice, and when these formats are.
- [Data engineering and storage](docs/data-engineering.md): measured index size, insert speed, WAL and compression for every format.
- [Specification](docs/ALGORITHMS.md): exact bit layouts and edge-case rules.

## Layout

```
c/          shared C99 core used by the database extensions (+ tests, benchmark)
go/         Go module   github.com/rahulbsw/idgenkit/go   (ulid, snowflake, nanoid)
rust/       Rust crate  idgenkit                           (no dependencies)
java/       Java 17+    io.github.rahulbsw:idgenkit        (Maven pom + plain Makefile)
python/     Python 3.9+ package idgenkit                   (pure Python)
postgres/   PostgreSQL extension (PGXS, tested on 14 to 18 in Docker, in CI)
mysql/      MySQL loadable functions (UDF), tested on MySQL 9.2 (macOS) and 8.0 (Ubuntu, in CI)
redis/      Redis module (self-contained module API header)
testdata/   shared conformance vectors + generator script
bench/      run_all.sh — runs every benchmark and writes bench/results/*.txt
```

## Quick start

```sh
make test             # all libraries + all database extensions
make test-libs        # only Python, Go, Rust, Java, C
make test-postgres PG_MAJOR=16
make bench            # full benchmark report -> bench/results/
```

Requirements per component: Go ≥ 1.22, Rust ≥ 1.74, JDK ≥ 17, Python ≥ 3.9, a C99
compiler; `redis-server` for Redis, `mysqld`/`mysql_config` for MySQL, and Docker
(or podman) for PostgreSQL (the extension is built inside the official
`postgres:<major>` image, so no local `pg_config` is needed).

## Libraries

### Go

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

s := nanoid.New()                   // 21 chars, URL alphabet
hex, err := nanoid.CustomAlphabet("0123456789abcdef", 12)
s = hex.Generate()
```

### Rust

```rust
use idgenkit::{nanoid, snowflake::Snowflake, ulid::{MonotonicGenerator, Ulid}};

let id = Ulid::new();                       // Display -> 26 chars
let parsed = Ulid::parse("01ARYZ6S41TSV4RRFFQ69G5FAV")?;
let mono = MonotonicGenerator::new();       // Sync
let next = mono.next()?;

let gen = Snowflake::new(7, 1288834974657)?;
let sf = gen.next_id()?;
let parts = gen.parse(sf);

let s = nanoid::nanoid();
let hex = nanoid::CustomAlphabet::new("0123456789abcdef", 12)?;
let s = hex.generate();
```

### Java

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

Build with Maven (`mvn package`) or without it: `make -C java test bench jar`
(uses `$JAVA_HOME` if set).

### Python

```python
from idgenkit import ULID, MonotonicULID, Snowflake, nanoid, custom_alphabet

id = ULID.generate()            # str(id), id.timestamp_ms, id.to_uuid()
parsed = ULID.parse("01ARYZ6S41TSV4RRFFQ69G5FAV")
mono = MonotonicULID()          # thread-safe
nxt = mono.next()

gen = Snowflake(machine_id=7, epoch_ms=1288834974657)
sf = gen.next_id()
parts = gen.parse(sf)           # SnowflakeParts(timestamp_ms, machine_id, sequence)

s = nanoid()
hex_id = custom_alphabet("0123456789abcdef", 12)
h = hex_id()
```

## Database extensions

### PostgreSQL

```sh
make -C postgres && make -C postgres install      # needs pg_config on PATH
./postgres/test.sh                                # or: build + test in Docker (PG_MAJOR=17 default)
```

```sql
CREATE EXTENSION idgenkit;

SELECT ulid_generate();                    -- text, 26 chars
SELECT ulid_generate_monotonic();          -- per-backend monotonic
SELECT ulid_generate_uuid();               -- ULID as native uuid (16 bytes, index-friendly)
SELECT ulid_to_uuid('01ARYZ6S41TSV4RRFFQ69G5FAV'), ulid_from_uuid(u), ulid_timestamp(x);

SELECT snowflake_generate();               -- bigint
SELECT snowflake_timestamp(id), snowflake_machine_id(id), snowflake_sequence(id);
SELECT snowflake_from_timestamp(now());    -- lower bound for range scans

SELECT nanoid_generate();                  -- 21 chars
SELECT nanoid_generate(12, '0123456789abcdef');

CREATE TABLE t (id uuid PRIMARY KEY DEFAULT ulid_generate_uuid(), ...);
```

Settings (in `postgresql.conf`):

```
shared_preload_libraries = 'idgenkit'   # recommended
idgenkit.machine_id = 7                 # 0..1023, default 1
idgenkit.snowflake_epoch_ms = '1288834974657'   # default 0 (Unix epoch)
```

Snowflake state lives in shared memory, so all backends of one server share a
single sequence and never collide. With `shared_preload_libraries` the segment
is allocated at startup (any PostgreSQL version) and the settings require a
restart. Without preloading, PostgreSQL 17+ creates the segment on first use via
the DSM registry and the settings are captured at that moment. On 14–16
without preloading, ULID and Nano ID work but `snowflake_generate()` raises an
error with a hint to preload.

Store ULID text in a `COLLATE "C"` column (or use `ulid_generate_uuid()`) so
that sort order matches time order under non-C locales.

### MySQL

```sh
make -C mysql                                         # uses mysql_config
cp mysql/build/idgenkit_udf.so "$(mysql_config --plugindir)"
mysql -u root < mysql/install.sql
./mysql/test.sh [--bench]                             # throwaway server in mysql/build/
```

```sql
SELECT ulid_generate(), ulid_generate_monotonic(), ulid_timestamp(u);
SELECT ulid_to_bin(u), bin_to_ulid(b);                -- BINARY(16) storage
SELECT snowflake_generate(), snowflake_generate(7), snowflake_generate(7, 1288834974657);
SELECT snowflake_timestamp(id), snowflake_machine_id(id), snowflake_sequence(id);
SELECT nanoid_generate(), nanoid_generate(12, '0123456789abcdef');
```

Snowflake state is process-wide (lock-free atomic), so every connection shares
one sequence per machine id. Monotonic ULID state is per thread. Note that
MySQL, like it does for `UUID()`, folds table-independent expressions inside an
aggregate: `SELECT COUNT(DISTINCT ulid_generate())` returns 1. Values in rows
and `INSERT ... SELECT` are generated per row as expected.

### Redis

```sh
make -C redis
redis-server --loadmodule ./redis/build/idgenkit.so MACHINE_ID 7 EPOCH_MS 1288834974657
./redis/test.sh && ./redis/bench.sh
```

```
ULID.GENERATE              -> "01J..."           ULID.TIME <ulid>       -> ms
ULID.MONOTONIC             -> "01J..."
SNOWFLAKE.GENERATE         -> (integer)          SNOWFLAKE.PARSE <id>   -> [ms, machine, seq]
NANOID.GENERATE [size [alphabet]]
```

The module is built against a minimal hand-written module API header
(`redismodule_min.h`), so no Redis source is needed.

## Benchmarks

Apple M-series (arm64, 14 cores), macOS, single thread unless noted. The complete
report is in `bench/results/`; regenerate it with `make bench`.

**Libraries (ns per ID, lower is better)**

<!-- bench-table libraries -->

|  | ULID | ULID monotonic | ULID parse | Snowflake | Nano ID (21) |
|---|---|---|---|---|---|
| C core | 48 | 15 | 12 | 243 | 194 |
| Rust | 56 | 26 | 7 | 244 | 200 |
| Go | 98 | 36 | 11 | 244 | 234 |
| Java 24 | 213 | 21 | 27 | 244 | 186 |
| Python 3.12 | 1,126 | 312 | 1,992 | 307 | 1,058 |

Source: [`bench/results/2026-09-25-darwin-arm64.txt`](https://github.com/rahulbsw/idgenkit/blob/main/bench/results/2026-09-25-darwin-arm64.txt), generated by `bench/doc_tables.py`.

<!-- /bench-table -->

**Databases**

<!-- bench-table databases -->

|  | ULID | ULID monotonic | ULID as `uuid` | Snowflake | Nano ID | Built-in reference |
|---|---|---|---|---|---|---|
| PostgreSQL 17 (ns per row) | 68 | 41 | 54 | 209 | 91 | `gen_random_uuid()` 489 |
| MySQL 9.2 (ns per call) | 82 | 53 | — | 272 | 227 | `UUID()` 53 |
| Redis 8.4 (requests/s) | 690k | 722k | — | 697k | 714k | `PING` 669k |

Source: [`bench/results/2026-09-25-darwin-arm64.txt`](https://github.com/rahulbsw/idgenkit/blob/main/bench/results/2026-09-25-darwin-arm64.txt), generated by `bench/doc_tables.py`.

<!-- /bench-table -->

How to read these:

- **Snowflake is capped at 4,096 IDs per millisecond per generator by design**
  (12-bit sequence), which is a floor of ≈ 244 ns/ID. Once a millisecond's
  sequence is used up, the generator waits for the next millisecond. Every
  native implementation reaches that ceiling. To go faster, use more machine ids.
- PostgreSQL numbers subtract the cost of `count(n)` over `generate_series`.
  For Snowflake, some of that overhead overlaps with the time spent waiting for
  the next millisecond, so the net figure can read below 244 ns. Wall time for
  1M rows was 278 ms, which respects the cap.
- Redis throughput (50 clients, pipeline depth 16) is bound by networking and
  command dispatch: every command runs at `PING` speed, so the generator cost
  doesn't show.
- The C, Rust and Go numbers are dominated by the OS CSPRNG call
  (`arc4random_buf` / `getrandom` / `crypto/rand`). Monotonic ULID is faster
  because it only increments the previous value within a millisecond.
- Parallel generation on macOS contends on the system random source's lock
  (Go `ulid.NewParallel` is 297 ns/op versus 98 ns single-threaded). Linux's
  `getrandom` scales better. A userspace CSPRNG would avoid this, at the cost of
  more code to audit and extra care around `fork()`.

## Compatibility notes

- **ULID:** full spec compliance. Decoding is case-insensitive and rejects
  `I L O U` as well as values above `7ZZZZZZZZZZZZZZZZZZZZZZZZZ`. Monotonic
  generators increment the random part within the same millisecond, keep doing
  so if the clock moves backwards, and return an error (without changing state)
  when the 80-bit random part would overflow.
- **Snowflake:** 42-bit ms timestamp | 10-bit machine id | 12-bit sequence.
  The default epoch is 0 and the default machine id is 1, matching the
  reference implementation. With epoch 0 the IDs pass the signed 64-bit maximum
  in 2039, and Java `long`, PostgreSQL `bigint` and MySQL `BIGINT` all reject
  them with an explicit error. Set a recent custom epoch (for example
  `1288834974657`, Twitter's) for new systems. IDs remain unique across clock
  regressions because the generator keeps issuing from its last timestamp.
- **Nano ID:** the same alphabet, default size and rejection-masking algorithm
  as the reference implementation, and byte-for-byte identical output for a
  given random stream (verified against `testdata/nanoid.txt`). The only change
  is that the reference's floating-point `1.6` step factor is replaced by exact
  integer arithmetic. Language libraries accept any 1–256 symbol alphabet,
  including Unicode; the C core (and so the database extensions) accepts ASCII
  alphabets only.

## Releasing

A single tag publishes every package at the same version:

```sh
scripts/version.sh set 0.2.0      # rewrites pom.xml, pyproject.toml, __init__.py, Cargo.toml
git commit -am "release 0.2.0" && git tag v0.2.0 && git push origin main v0.2.0
```

`.github/workflows/release.yml` then:
1. Checks that every manifest matches the tag and runs the full CI suite.
2. Publishes each package:
   - Python to PyPI as `idgenkit`
   - Rust to crates.io as `idgenkit`
   - Java to Maven Central as `io.github.rahulbsw:idgenkit`
   - Go by pushing the `go/v0.2.0` tag that `proxy.golang.org` serves the module from
3. Creates a GitHub Release. It includes the Redis, MySQL and PostgreSQL 14–18
   binaries for Linux amd64/arm64, the Python and Java artifacts, `SHA256SUMS`,
   and build-provenance attestations.

Running the workflow manually from the Actions tab is a dry run: it builds and
uploads everything but publishes nothing.

One-time setup (GitHub → Settings → Environments and Secrets):

| Registry | Environment | Setup |
|---|---|---|
| PyPI | `pypi` | Add a trusted publisher on pypi.org: repo `rahulbsw/idgenkit`, workflow `release.yml`, environment `pypi`. No secret needed. |
| crates.io | `crates-io` (deploys from `v*` tags only) | First release: environment secret `CARGO_REGISTRY_TOKEN` (scope `publish-new`), set with `gh secret set CARGO_REGISTRY_TOKEN --env crates-io`, not as a repository secret. Then enable trusted publishing for `release.yml` on crates.io and delete the secret. |
| Maven Central | `maven-central` | Secrets `MAVEN_CENTRAL_USERNAME` / `MAVEN_CENTRAL_PASSWORD` (a Central Portal user token), `MAVEN_GPG_PRIVATE_KEY` (armored) and `MAVEN_GPG_PASSPHRASE`. The GPG public key must be on a keyserver. |
| Go | none | Nothing to configure. |

## Security

- Randomness comes only from the OS CSPRNG: `crypto/rand` (Go), `os.urandom`
  (Python), `SecureRandom` DRBG (Java), and `arc4random_buf` / `getrandom` /
  `getentropy` / `BCryptGenRandom` (Rust and C). There is no userspace pooling,
  so there's nothing to reseed after `fork()`.
- The C code bounds every write, formats error messages with `snprintf`, and is
  tested under AddressSanitizer and UndefinedBehaviorSanitizer (in the Linux
  Docker build).
- IDs are not secrets. ULID and Snowflake reveal their creation time, and
  Snowflake also reveals the machine id and is guessable. Use Nano ID (≥ 21
  characters) or a ULID's random part when an ID must be hard to guess.
