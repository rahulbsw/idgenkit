# idgenkit (Rust)

Dependency-free ULID, Snowflake and Nano ID generators. Randomness comes
directly from the OS (`arc4random_buf`, `getentropy` or `BCryptGenRandom`).

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

Implementations for Java, Go and Python, plus PostgreSQL, MySQL and Redis
extensions, live in the same repository:
<https://github.com/rahulbsw/idgenkit>.
