//! RFC 9562 UUIDs: version 4 (122 random bits) and version 7 (48-bit
//! millisecond timestamp + 74 random bits). The standard library has no UUID
//! type, so this is a small dependency-free one.

use crate::{now_ms, rng, Error};
use std::fmt;
use std::str::FromStr;
use std::sync::Mutex;

/// Largest UUIDv7 timestamp in milliseconds.
pub const MAX_TIME: u64 = (1 << 48) - 1;
/// Length of the canonical string form.
pub const ENCODED_LEN: usize = 36;

const HEX: &[u8; 16] = b"0123456789abcdef";
const RAND_B_MASK: u128 = (1 << 62) - 1;
const RAND_MAX: u128 = (1 << 74) - 1;

/// A 128-bit UUID. Ordering matches both byte order and string order.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Uuid(pub u128);

fn with_version(v: u128, version: u8) -> Uuid {
    let v = (v & !(0xF << 76)) | ((version as u128) << 76);
    Uuid((v & !(0b11 << 62)) | (0b10 << 62))
}

impl Uuid {
    /// Random (version 4) UUID from OS randomness.
    pub fn new_v4() -> Uuid {
        let mut r = [0u8; 16];
        rng::fill(&mut r);
        Uuid::v4_from_bytes(r)
    }

    /// Sets the version and variant bits of 16 random bytes.
    pub fn v4_from_bytes(random: [u8; 16]) -> Uuid {
        with_version(u128::from_be_bytes(random), 4)
    }

    /// Time-ordered (version 7) UUID for the current time.
    pub fn new_v7() -> Result<Uuid, Error> {
        let mut r = [0u8; 10];
        rng::fill(&mut r);
        Uuid::v7_from_parts(now_ms(), r)
    }

    /// Builds a UUIDv7; the version and variant bits overwrite 6 of the random bits.
    pub fn v7_from_parts(timestamp_ms: u64, random: [u8; 10]) -> Result<Uuid, Error> {
        if timestamp_ms > MAX_TIME {
            return Err(Error::TimeRange);
        }
        let mut b = [0u8; 16];
        b[..6].copy_from_slice(&timestamp_ms.to_be_bytes()[2..]);
        b[6..].copy_from_slice(&random);
        Ok(with_version(u128::from_be_bytes(b), 7))
    }

    /// The version nibble (4 or 7 for UUIDs made here).
    pub fn version(self) -> u8 {
        (self.0 >> 76) as u8 & 0xF
    }

    /// Unix timestamp in milliseconds of a version 7 UUID.
    pub fn timestamp_ms(self) -> Result<u64, Error> {
        if self.version() != 7 {
            return Err(Error::NotUuidV7);
        }
        Ok((self.0 >> 80) as u64)
    }

    pub fn to_bytes(self) -> [u8; 16] {
        self.0.to_be_bytes()
    }

    pub fn from_bytes(bytes: [u8; 16]) -> Uuid {
        Uuid(u128::from_be_bytes(bytes))
    }

    /// Encode the lowercase 8-4-4-4-12 form into a stack buffer without allocating.
    pub fn encode(self, out: &mut [u8; ENCODED_LEN]) {
        let mut o = 0;
        for (i, b) in self.to_bytes().into_iter().enumerate() {
            if matches!(i, 4 | 6 | 8 | 10) {
                out[o] = b'-';
                o += 1;
            }
            out[o] = HEX[(b >> 4) as usize];
            out[o + 1] = HEX[(b & 15) as usize];
            o += 2;
        }
    }

    /// Case-insensitive 8-4-4-4-12 form only: braces, `urn:uuid:` and the
    /// 32-digit form without hyphens are rejected.
    pub fn parse(s: &str) -> Result<Uuid, Error> {
        let b = s.as_bytes();
        if b.len() != ENCODED_LEN {
            return Err(Error::InvalidUuid);
        }
        let mut v: u128 = 0;
        for (i, &c) in b.iter().enumerate() {
            if matches!(i, 8 | 13 | 18 | 23) {
                if c != b'-' {
                    return Err(Error::InvalidUuid);
                }
                continue;
            }
            let d = (c as char).to_digit(16).ok_or(Error::InvalidUuid)?;
            v = (v << 4) | d as u128;
        }
        Ok(Uuid(v))
    }

    fn rand74(self) -> u128 {
        ((self.0 >> 64) & 0xFFF) << 62 | (self.0 & RAND_B_MASK)
    }

    fn with_rand74(self, r: u128) -> Uuid {
        let time = self.0 >> 80 << 80;
        Uuid(time | 7 << 76 | (r >> 62) << 64 | 0b10 << 62 | (r & RAND_B_MASK))
    }
}

impl fmt::Display for Uuid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut buf = [0u8; ENCODED_LEN];
        self.encode(&mut buf);
        // SAFETY: every byte is an ASCII hex digit or '-'.
        f.write_str(unsafe { std::str::from_utf8_unchecked(&buf) })
    }
}

impl fmt::Debug for Uuid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Uuid({self})")
    }
}

impl FromStr for Uuid {
    type Err = Error;
    fn from_str(s: &str) -> Result<Uuid, Error> {
        Uuid::parse(s)
    }
}

/// Thread-safe generator of strictly increasing UUIDv7s.
///
/// Within the same millisecond (or if the clock moves backwards) the previous
/// 74 random bits are incremented instead of drawing fresh randomness.
#[derive(Default)]
pub struct MonotonicV7Generator {
    last: Mutex<Option<Uuid>>,
}

impl MonotonicV7Generator {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn next(&self) -> Result<Uuid, Error> {
        self.next_at(now_ms(), rng::fill)
    }

    pub(crate) fn next_at(&self, now: u64, fill: impl FnOnce(&mut [u8])) -> Result<Uuid, Error> {
        let mut last = self.last.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(prev) = *last {
            if now <= (prev.0 >> 80) as u64 {
                let r = prev.rand74();
                if r == RAND_MAX {
                    return Err(Error::MonotonicOverflow);
                }
                let next = prev.with_rand74(r + 1);
                *last = Some(next);
                return Ok(next);
            }
        }
        let mut r = [0u8; 10];
        if now <= MAX_TIME {
            fill(&mut r);
        }
        let next = Uuid::v7_from_parts(now, r)?;
        *last = Some(next);
        Ok(next)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_vectors;

    #[test]
    fn monotonic_vectors() {
        let rows = test_vectors("uuid7_monotonic.txt");
        assert!(rows.len() > 10);
        let mut g = MonotonicV7Generator::new();
        for r in &rows {
            if r[0] == "reset" {
                g = MonotonicV7Generator::new();
                continue;
            }
            let now: u64 = r[1].parse().unwrap();
            let mut drew = false;
            let got = g.next_at(now, |buf| {
                drew = true;
                if r[2] != "-" {
                    let rnd = u128::from_str_radix(&r[2], 16).unwrap();
                    buf.copy_from_slice(&rnd.to_be_bytes()[6..]);
                }
            });
            assert!(!(r[2] == "-" && drew), "{r:?}: drew randomness");
            match (r[3].as_str(), got) {
                ("error", Ok(u)) => panic!("{r:?}: got {u}, want an error"),
                ("error", Err(_)) => {}
                (want, got) => assert_eq!(got.map(|u| u.to_string()), Ok(want.to_string()), "{r:?}"),
            }
        }
    }
}
