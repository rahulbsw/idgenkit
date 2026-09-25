//! Dependency-free ULID, UUIDv4/v7, Snowflake and Nano ID generators.
//!
//! ```
//! let id = idgenkit::ulid::Ulid::new().to_string();
//! assert_eq!(id.len(), 26);
//!
//! let u = idgenkit::uuid::Uuid::new_v7().unwrap();
//! assert_eq!(u.version(), 7);
//!
//! let sf = idgenkit::snowflake::Snowflake::new(1, 0).unwrap();
//! assert!(sf.next_id().unwrap() > 0);
//!
//! assert_eq!(idgenkit::nanoid::nanoid().len(), 21);
//! ```

pub mod nanoid;
pub mod rng;
pub mod snowflake;
pub mod ulid;
pub mod uuid;

use std::fmt;

/// Errors returned by the generators and parsers in this crate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    InvalidLength,
    InvalidChar,
    Overflow,
    TimeRange,
    MonotonicOverflow,
    MachineId,
    Sequence,
    Alphabet,
    Size,
    InvalidUuid,
    NotUuidV7,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::InvalidLength => "encoded ULID must be 26 characters",
            Error::InvalidChar => "invalid ULID character",
            Error::Overflow => "ULID overflows 128 bits",
            Error::TimeRange => "timestamp out of range",
            Error::MonotonicOverflow => "monotonic random component overflow",
            Error::MachineId => "machine id must be in [0, 1023]",
            Error::Sequence => "sequence must be in [0, 4095]",
            Error::Alphabet => "alphabet must contain between 1 and 256 symbols",
            Error::Size => "size must be >= 1",
            Error::InvalidUuid => "UUID must be 36 characters in 8-4-4-4-12 hex form",
            Error::NotUuidV7 => "not a version 7 UUID",
        })
    }
}

impl std::error::Error for Error {}

pub(crate) fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

#[cfg(test)]
pub(crate) fn test_vectors(name: &str) -> Vec<Vec<String>> {
    let path = format!("{}/../testdata/{name}", env!("CARGO_MANIFEST_DIR"));
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|l| l.split_whitespace().map(String::from).collect())
        .collect()
}
