use std::collections::HashSet;
use std::sync::Arc;
use std::thread;
use idgenkit::nanoid::{self, CustomAlphabet, URL_ALPHABET};
use idgenkit::snowflake::{self, Snowflake};
use idgenkit::ulid::{MonotonicGenerator, Ulid, MAX_TIME};
use idgenkit::uuid::{MonotonicV7Generator, Uuid};
use idgenkit::Error;

fn vectors(name: &str) -> Vec<Vec<String>> {
    let path = format!("{}/../testdata/{name}", env!("CARGO_MANIFEST_DIR"));
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|l| l.split_whitespace().map(String::from).collect())
        .collect()
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

#[test]
fn ulid_vectors() {
    for r in vectors("ulid.txt") {
        let ts: u64 = r[0].parse().unwrap();
        let rnd = u128::from_str_radix(&r[1], 16).unwrap();
        let u = Ulid::from_parts(ts, rnd).unwrap();
        assert_eq!(u.to_string(), r[2]);
        let p: Ulid = r[2].parse().unwrap();
        assert_eq!((p, p.timestamp_ms(), p.random()), (u, ts, rnd));
        assert_eq!(Ulid::parse(&r[2].to_lowercase()).unwrap(), u);
        assert_eq!(Ulid::from_bytes(u.to_bytes()), u);
    }
}

#[test]
fn ulid_invalid() {
    for r in vectors("ulid_invalid.txt") {
        assert!(Ulid::parse(&r[0]).is_err(), "{} should fail", r[0]);
    }
    assert_eq!(Ulid::parse(""), Err(Error::InvalidLength));
    assert_eq!(Ulid::from_parts(MAX_TIME + 1, 0), Err(Error::TimeRange));
    assert_eq!(Ulid::from_parts(0, 1 << 80), Err(Error::Overflow));
}

#[test]
fn ulid_unique() {
    let set: HashSet<Ulid> = (0..10_000).map(|_| Ulid::new()).collect();
    assert_eq!(set.len(), 10_000);
}

#[test]
fn ulid_monotonic() {
    let g = MonotonicGenerator::new();
    let ids: Vec<Ulid> = (0..50_000).map(|_| g.next().unwrap()).collect();
    assert!(ids.windows(2).all(|w| w[0] < w[1]));
    let strs: Vec<String> = ids.iter().map(|u| u.to_string()).collect();
    assert!(strs.windows(2).all(|w| w[0] < w[1]));
}

#[test]
fn ulid_monotonic_concurrent() {
    let g = Arc::new(MonotonicGenerator::new());
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let g = g.clone();
            thread::spawn(move || (0..5000).map(|_| g.next().unwrap()).collect::<Vec<_>>())
        })
        .collect();
    let all: HashSet<Ulid> = handles.into_iter().flat_map(|h| h.join().unwrap()).collect();
    assert_eq!(all.len(), 40_000);
}

#[test]
fn uuid_vectors() {
    let rows = vectors("uuid.txt");
    assert!(rows.len() > 10);
    for r in rows {
        let want = r.last().unwrap();
        let (u, ms) = match r[0].as_str() {
            "v4" => (Ok(Uuid::v4_from_bytes(hex(&r[1]).try_into().unwrap())), None),
            "v7" => {
                let ms: u64 = r[1].parse().unwrap();
                (Uuid::v7_from_parts(ms, hex(&r[2]).try_into().unwrap()), Some(ms))
            }
            other => panic!("bad line kind {other}"),
        };
        if want == "error" {
            assert_eq!(u, Err(Error::TimeRange), "{r:?}");
            continue;
        }
        let u = u.unwrap();
        assert_eq!(&u.to_string(), want);
        let p: Uuid = want.parse().unwrap();
        assert_eq!(p, u);
        assert_eq!(Uuid::parse(&want.to_uppercase()), Ok(u));
        assert_eq!(Uuid::from_bytes(u.to_bytes()), u);
        assert_eq!(p.version().to_string(), r[0][1..]);
        assert_eq!(p.timestamp_ms().ok(), ms);
    }
}

#[test]
fn uuid_invalid() {
    let path = format!("{}/../testdata/uuid_invalid.txt", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(path).unwrap();
    let rows: Vec<&str> = text.lines().filter(|l| !l.starts_with('#')).map(|l| &l[1..l.len() - 1]).collect();
    assert!(rows.len() > 10);
    for s in rows {
        assert_eq!(Uuid::parse(s), Err(Error::InvalidUuid), "{s:?} should fail");
    }
    assert_eq!(Uuid::new_v4().timestamp_ms(), Err(Error::NotUuidV7));
}

#[test]
fn uuid_generate() {
    let set: HashSet<Uuid> = (0..10_000).flat_map(|_| [Uuid::new_v4(), Uuid::new_v7().unwrap()]).collect();
    assert_eq!(set.len(), 20_000);
    assert!(set.iter().all(|u| matches!(u.version(), 4 | 7) && u.to_bytes()[8] & 0xC0 == 0x80));
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as u64;
    let ts = Uuid::new_v7().unwrap().timestamp_ms().unwrap();
    assert!((now..now + 5000).contains(&ts));
}

#[test]
fn uuid_monotonic() {
    let g = Arc::new(MonotonicV7Generator::new());
    let ids: Vec<Uuid> = (0..50_000).map(|_| g.next().unwrap()).collect();
    assert!(ids.windows(2).all(|w| w[0] < w[1]));
    let strs: Vec<String> = ids.iter().map(|u| u.to_string()).collect();
    assert!(strs.windows(2).all(|w| w[0] < w[1]));
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let g = g.clone();
            thread::spawn(move || (0..5000).map(|_| g.next().unwrap()).collect::<Vec<_>>())
        })
        .collect();
    let all: HashSet<Uuid> = handles.into_iter().flat_map(|h| h.join().unwrap()).collect();
    assert_eq!(all.len(), 40_000);
}

#[test]
fn snowflake_vectors() {
    for r in vectors("snowflake.txt") {
        let n: Vec<u64> = r.iter().map(|s| s.parse().unwrap()).collect();
        let (epoch, mid, seq, ts, id) = (n[0], n[1] as u16, n[2] as u16, n[3], n[4]);
        assert_eq!(snowflake::compose(ts, mid, seq, epoch), Ok(id));
        assert_eq!(snowflake::parse(id, epoch), snowflake::Parts { timestamp_ms: ts, machine_id: mid, sequence: seq });
    }
}

#[test]
fn snowflake_validation() {
    assert!(Snowflake::new(1024, 0).is_err());
    assert_eq!(snowflake::compose(0, 0, 4096, 0), Err(Error::Sequence));
    assert_eq!(snowflake::compose(5, 0, 0, 10), Err(Error::TimeRange));
    assert_eq!(Snowflake::new(1, u64::MAX / 2).unwrap().next_id(), Err(Error::TimeRange));
}

#[test]
fn snowflake_ordered_unique() {
    let g = Snowflake::new(7, 1_600_000_000_000).unwrap();
    let ids: Vec<u64> = (0..100_000).map(|_| g.next_id().unwrap()).collect();
    assert!(ids.windows(2).all(|w| w[0] < w[1]));
    let p = g.parse(*ids.last().unwrap());
    assert_eq!(p.machine_id, 7);
    assert!(p.timestamp_ms > 1_600_000_000_000);
}

#[test]
fn snowflake_concurrent() {
    let g = Arc::new(Snowflake::new(3, 0).unwrap());
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let g = g.clone();
            thread::spawn(move || (0..20_000).map(|_| g.next_id().unwrap()).collect::<Vec<_>>())
        })
        .collect();
    let all: HashSet<u64> = handles.into_iter().flat_map(|h| h.join().unwrap()).collect();
    assert_eq!(all.len(), 160_000);
}

#[test]
fn nanoid_vectors() {
    for r in vectors("nanoid.txt") {
        let size: usize = r[1].parse().unwrap();
        let stream = hex(&r[2]);
        let mut pos = 0;
        let g = CustomAlphabet::new(&r[0], size).unwrap();
        let id = g.generate_with(size, |buf| {
            buf.copy_from_slice(&stream[pos..pos + buf.len()]);
            pos += buf.len();
        });
        assert_eq!(id, r[3], "alphabet {}", r[0]);
    }
}

#[test]
fn nanoid_default() {
    let set: HashSet<String> = (0..10_000).map(|_| nanoid::nanoid()).collect();
    assert_eq!(set.len(), 10_000);
    for id in set.iter().take(100) {
        assert_eq!(id.len(), 21);
        assert!(id.chars().all(|c| URL_ALPHABET.contains(c)));
    }
    assert_eq!(nanoid::nanoid_with_size(64).unwrap().len(), 64);
    assert_eq!(nanoid::nanoid_with_size(0), Err(Error::Size));
}

#[test]
fn nanoid_custom() {
    let g = CustomAlphabet::new("0123456789", 12).unwrap();
    let id = g.generate();
    assert!(id.len() == 12 && id.chars().all(|c| c.is_ascii_digit()));
    let u = CustomAlphabet::new("αβγδ", 8).unwrap().generate();
    assert!(u.chars().count() == 8 && u.chars().all(|c| "αβγδ".contains(c)));
    assert!(CustomAlphabet::new("", 5).is_err());
    assert!(CustomAlphabet::new(&"x".repeat(257), 5).is_err());
}

#[test]
fn nanoid_distribution() {
    let id = CustomAlphabet::new("abcdefghij", 100_000).unwrap().generate();
    for c in "abcdefghij".chars() {
        let n = id.chars().filter(|&x| x == c).count();
        assert!((9_500..=10_500).contains(&n), "{c}: {n}");
    }
}
