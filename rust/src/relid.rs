//! Relative IDs: sortable, unique IDs in which every ID generated for the same
//! key starts with the same tag.
//!
//! ```text
//! 3F8KQZ-01J8Y4Q9M3-X3B0N6E3P8
//! tag    48-bit ms  50-bit random / counter
//! ```
//!
//! The tag is the top 30 bits of `HMAC-SHA-256(secret, BE32(len(salt)) ‖ salt ‖ key)`.
//! The secret keeps tags unguessable; the optional salt gives the same key
//! unrelated tags in different contexts.

use crate::sha256::Hmac;
use crate::{now_ms, rng, Error};
use std::fmt;
use std::str::FromStr;
use std::sync::Mutex;

/// Length of the canonical text form.
pub const ENCODED_LEN: usize = 28;
/// Length of a tag's text form.
pub const TAG_LEN: usize = 6;
/// Shortest accepted secret, in bytes.
pub const MIN_SECRET_LEN: usize = 16;
pub const MAX_TAG: u32 = (1 << 30) - 1;
pub const MAX_TIME: u64 = (1 << 48) - 1;
pub const MAX_RANDOM: u64 = (1 << 50) - 1;

const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

const DECODE: [i8; 256] = {
    let mut t = [-1i8; 256];
    let mut i = 0;
    while i < 32 {
        t[ALPHABET[i] as usize] = i as i8;
        t[(ALPHABET[i] | 0x20) as usize] = i as i8;
        i += 1;
    }
    t
};

/// A decoded relative ID. Ordering matches the text form.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Parts {
    pub tag: u32,
    pub timestamp_ms: u64,
    pub random: u64,
}

fn put(dst: &mut [u8], mut v: u64) {
    for b in dst.iter_mut().rev() {
        *b = ALPHABET[(v & 31) as usize];
        v >>= 5;
    }
}

fn get(s: &[u8]) -> Option<u64> {
    s.iter().try_fold(0u64, |v, &c| {
        let d = DECODE[c as usize];
        (d >= 0).then_some(v << 5 | d as u64)
    })
}

/// The 6-character text form of a 30-bit tag.
pub fn encode_tag(tag: u32) -> String {
    let mut b = [0u8; TAG_LEN];
    put(&mut b, (tag & MAX_TAG) as u64);
    String::from_utf8(b.to_vec()).expect("ASCII")
}

impl Parts {
    pub fn new(tag: u32, timestamp_ms: u64, random: u64) -> Result<Parts, Error> {
        if tag > MAX_TAG || timestamp_ms > MAX_TIME || random > MAX_RANDOM {
            return Err(Error::OutOfRange);
        }
        Ok(Parts {
            tag,
            timestamp_ms,
            random,
        })
    }

    /// The 6-character tag every ID with this tag starts with.
    pub fn tag_text(&self) -> String {
        encode_tag(self.tag)
    }

    /// Encode the 28-character form into a stack buffer without allocating.
    pub fn encode(&self) -> [u8; ENCODED_LEN] {
        let mut b = [b'-'; ENCODED_LEN];
        put(&mut b[0..6], self.tag as u64);
        put(&mut b[7..17], self.timestamp_ms);
        put(&mut b[18..28], self.random);
        b
    }

    /// Parse the case-insensitive 28-character form, or the same 26 characters
    /// without hyphens.
    pub fn parse(s: &str) -> Result<Parts, Error> {
        let b = s.as_bytes();
        let (tag, ms, rnd) = match b.len() {
            ENCODED_LEN if b[6] == b'-' && b[17] == b'-' => (&b[0..6], &b[7..17], &b[18..28]),
            26 => (&b[0..6], &b[6..16], &b[16..26]),
            _ => return Err(Error::InvalidRelativeId),
        };
        match (get(tag), get(ms), get(rnd)) {
            (Some(t), Some(m), Some(r)) if m <= MAX_TIME => Ok(Parts {
                tag: t as u32,
                timestamp_ms: m,
                random: r,
            }),
            _ => Err(Error::InvalidRelativeId),
        }
    }
}

impl fmt::Display for Parts {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(std::str::from_utf8(&self.encode()).expect("ASCII"))
    }
}

impl FromStr for Parts {
    type Err = Error;
    fn from_str(s: &str) -> Result<Parts, Error> {
        Parts::parse(s)
    }
}

/// Encodes a tag, millisecond timestamp and 50-bit random value.
pub fn from_parts(tag: u32, timestamp_ms: u64, random: u64) -> Result<String, Error> {
    Parts::new(tag, timestamp_ms, random).map(|p| p.to_string())
}

fn random50(fill: impl FnOnce(&mut [u8])) -> u64 {
    let mut b = [0u8; 8];
    fill(&mut b[1..]);
    u64::from_be_bytes(b) & MAX_RANDOM
}

/// Generates relative IDs for one secret and salt. Thread-safe.
///
/// `generate` draws 50 fresh random bits per ID. `monotonic` shares one
/// counter across all keys, so each key's IDs strictly increase and no two IDs
/// from this generator are equal.
pub struct RelativeId {
    mac: Hmac,
    state: Mutex<(u64, u64, bool)>,
}

impl RelativeId {
    /// The secret must be at least [`MIN_SECRET_LEN`] bytes and must come from
    /// configuration or a key store, never from source code.
    pub fn new(secret: &[u8], salt: &str) -> Result<RelativeId, Error> {
        if secret.len() < MIN_SECRET_LEN {
            return Err(Error::Secret);
        }
        let len = u32::try_from(salt.len()).map_err(|_| Error::OutOfRange)?;
        let mut mac = Hmac::new(secret);
        mac.inner.update(&len.to_be_bytes());
        mac.inner.update(salt.as_bytes());
        Ok(RelativeId {
            mac,
            state: Mutex::new((0, 0, false)),
        })
    }

    /// The 30-bit tag for `key`.
    pub fn tag_value(&self, key: &str) -> u32 {
        let m = self.mac.mac(key.as_bytes());
        u32::from_be_bytes([m[0], m[1], m[2], m[3]]) >> 2
    }

    /// The 6-character tag that starts every ID for `key`.
    pub fn tag(&self, key: &str) -> String {
        encode_tag(self.tag_value(key))
    }

    /// A new ID for `key` with 50 fresh random bits.
    pub fn generate(&self, key: &str) -> Result<String, Error> {
        from_parts(self.tag_value(key), now_ms(), random50(rng::fill))
    }

    /// A new ID for `key` from the shared counter. Fails with
    /// [`Error::MonotonicOverflow`] if the counter is exhausted within one millisecond.
    pub fn monotonic(&self, key: &str) -> Result<String, Error> {
        self.monotonic_at(self.tag_value(key), now_ms(), rng::fill)
    }

    pub(crate) fn monotonic_at(
        &self,
        tag: u32,
        now: u64,
        fill: impl FnOnce(&mut [u8]),
    ) -> Result<String, Error> {
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let (last_ms, last_rand, primed) = &mut *st;
        if *primed && now <= *last_ms {
            if *last_rand == MAX_RANDOM {
                return Err(Error::MonotonicOverflow);
            }
            *last_rand += 1;
        } else {
            if now > MAX_TIME {
                return Err(Error::TimeRange);
            }
            *last_ms = now;
            *last_rand = random50(fill);
            *primed = true;
        }
        from_parts(tag, *last_ms, *last_rand)
    }
}

impl Drop for RelativeId {
    fn drop(&mut self) {
        self.mac.wipe();
    }
}

impl fmt::Debug for RelativeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("RelativeId { .. }")
    }
}

#[cfg(test)]
mod tests {
    use super::RelativeId;

    fn unhex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn monotonic_vectors() {
        let secret = b"test-only-secret-0123456789";
        let rows = crate::test_vectors("relid_monotonic.txt");
        assert!(rows.len() > 10);
        let mut gen = RelativeId::new(secret, "").unwrap();
        for r in rows {
            if r[0] == "reset" {
                gen = RelativeId::new(secret, "").unwrap();
                continue;
            }
            let (tag, now) = (r[1].parse().unwrap(), r[2].parse().unwrap());
            let mut drew = false;
            let got = gen.monotonic_at(tag, now, |b| {
                drew = true;
                if r[3] != "-" {
                    b.copy_from_slice(&unhex(&r[3]));
                }
            });
            match r[4].as_str() {
                "error" => assert!(got.is_err(), "{r:?}: {got:?}"),
                want => assert_eq!(got.as_deref(), Ok(want), "{r:?}"),
            }
            assert!(!(r[3] == "-" && drew), "{r:?}: drew randomness");
        }
    }
}
