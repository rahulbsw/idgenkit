//! ULID: 48-bit millisecond timestamp + 80 bits of randomness, encoded as 26
//! characters of Crockford base32. See <https://github.com/ulid/spec>.

use crate::{now_ms, rng, Error};
use std::fmt;
use std::str::FromStr;
use std::sync::Mutex;

const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const RANDOM_BITS: u32 = 80;
const RANDOM_MASK: u128 = (1 << RANDOM_BITS) - 1;

/// Largest representable timestamp in milliseconds.
pub const MAX_TIME: u64 = (1 << 48) - 1;
/// Length of the canonical string form.
pub const ENCODED_LEN: usize = 26;

const DECODE: [u8; 256] = {
    let mut t = [0xFFu8; 256];
    let mut i = 0;
    while i < 32 {
        let c = ALPHABET[i];
        t[c as usize] = i as u8;
        if c.is_ascii_uppercase() {
            t[c.to_ascii_lowercase() as usize] = i as u8;
        }
        i += 1;
    }
    t
};

/// A 128-bit ULID. Ordering matches both creation time and string order.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Ulid(pub u128);

impl Ulid {
    /// Current time plus 80 bits of OS randomness.
    pub fn new() -> Ulid {
        let mut r = [0u8; 16];
        rng::fill(&mut r[6..]);
        Ulid(((now_ms() as u128) << RANDOM_BITS) | u128::from_be_bytes(r))
    }

    pub fn from_parts(timestamp_ms: u64, random: u128) -> Result<Ulid, Error> {
        if timestamp_ms > MAX_TIME {
            return Err(Error::TimeRange);
        }
        if random > RANDOM_MASK {
            return Err(Error::Overflow);
        }
        Ok(Ulid(((timestamp_ms as u128) << RANDOM_BITS) | random))
    }

    pub fn timestamp_ms(self) -> u64 {
        (self.0 >> RANDOM_BITS) as u64
    }

    pub fn random(self) -> u128 {
        self.0 & RANDOM_MASK
    }

    pub fn to_bytes(self) -> [u8; 16] {
        self.0.to_be_bytes()
    }

    pub fn from_bytes(bytes: [u8; 16]) -> Ulid {
        Ulid(u128::from_be_bytes(bytes))
    }

    /// Encode into a stack buffer without allocating.
    pub fn encode(self, out: &mut [u8; ENCODED_LEN]) {
        let mut v = self.0;
        for slot in out.iter_mut().rev() {
            *slot = ALPHABET[(v & 31) as usize];
            v >>= 5;
        }
    }

    pub fn parse(s: &str) -> Result<Ulid, Error> {
        let b = s.as_bytes();
        if b.len() != ENCODED_LEN {
            return Err(Error::InvalidLength);
        }
        let mut v: u128 = 0;
        for (i, &c) in b.iter().enumerate() {
            let d = DECODE[c as usize];
            if d == 0xFF {
                return Err(Error::InvalidChar);
            }
            if i == 0 && d > 7 {
                return Err(Error::Overflow);
            }
            v = (v << 5) | d as u128;
        }
        Ok(Ulid(v))
    }
}

impl fmt::Display for Ulid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut buf = [0u8; ENCODED_LEN];
        self.encode(&mut buf);
        // SAFETY: every byte comes from the ASCII alphabet.
        f.write_str(unsafe { std::str::from_utf8_unchecked(&buf) })
    }
}

impl fmt::Debug for Ulid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Ulid({self})")
    }
}

impl FromStr for Ulid {
    type Err = Error;
    fn from_str(s: &str) -> Result<Ulid, Error> {
        Ulid::parse(s)
    }
}

/// Thread-safe generator of strictly increasing ULIDs.
///
/// Within the same millisecond (or if the clock moves backwards) the previous
/// random component is incremented instead of drawing fresh randomness.
#[derive(Default)]
pub struct MonotonicGenerator {
    last: Mutex<Option<Ulid>>,
}

impl MonotonicGenerator {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn next(&self) -> Result<Ulid, Error> {
        self.next_at(now_ms(), rng::fill)
    }

    pub(crate) fn next_at(&self, now: u64, fill: impl FnOnce(&mut [u8])) -> Result<Ulid, Error> {
        let mut last = self.last.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(prev) = *last {
            if now <= prev.timestamp_ms() {
                if prev.random() == RANDOM_MASK {
                    return Err(Error::MonotonicOverflow);
                }
                let next = Ulid(prev.0 + 1);
                *last = Some(next);
                return Ok(next);
            }
        }
        if now > MAX_TIME {
            return Err(Error::TimeRange);
        }
        let mut r = [0u8; 16];
        fill(&mut r[6..]);
        let next = Ulid(((now as u128) << RANDOM_BITS) | u128::from_be_bytes(r));
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
        let rows = test_vectors("ulid_monotonic.txt");
        assert!(rows.len() > 10);
        let mut g = MonotonicGenerator::new();
        for r in &rows {
            if r[0] == "reset" {
                g = MonotonicGenerator::new();
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
