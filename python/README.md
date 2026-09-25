# idgenkit (Python)

Dependency-free ULID, Snowflake and Nano ID generators in pure Python (3.9+).
Randomness comes from `os.urandom`.

```sh
pip install idgenkit
```

```python
from idgenkit import ULID, MonotonicULID, Snowflake, nanoid, custom_alphabet

id = ULID.generate()            # str(id), id.timestamp_ms, id.to_uuid()
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

Implementations for Java, Rust and Go, plus PostgreSQL, MySQL and Redis
extensions, live in the same repository:
<https://github.com/rahulbsw/idgenkit>.
