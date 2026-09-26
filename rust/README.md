# idgenkit (Rust)

Dependency-free ULID, UUIDv4/v7, relative ID, Snowflake and Nano ID generators. Randomness
comes directly from the OS (`arc4random_buf`, `getentropy` or `BCryptGenRandom`).

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
let ms = v7.timestamp_ms()?;
let next7 = MonotonicV7Generator::new().next()?;

let gen = Snowflake::new(7, 1288834974657)?;
let sf = gen.next_id()?;
let parts = gen.parse(sf);

let s = nanoid::nanoid();
let hex = nanoid::CustomAlphabet::new("0123456789abcdef", 12)?;
let s = hex.generate();

use idgenkit::relid::{Parts, RelativeId};
// The secret (at least 16 bytes) comes from your secret store, never from source code.
let secret = std::env::var("IDGENKIT_RELID_SECRET")?;
let rel = RelativeId::new(secret.as_bytes(), "orders")?; // Sync
let r = rel.generate("customer-42")?;       // same 6-char tag for every customer-42 ID
let prefix = rel.tag("customer-42");        // range-scan prefix
let p = Parts::parse(&r)?;                  // tag, timestamp_ms, random
```

Implementations for Java, Go and Python, plus PostgreSQL, MySQL and Redis
extensions, live in the same repository:
<https://github.com/rahulbsw/idgenkit>.
