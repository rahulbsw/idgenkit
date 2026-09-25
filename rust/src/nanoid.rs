//! Nano ID: compact, URL-friendly random string IDs following
//! <https://github.com/ai/nanoid>. Custom alphabets use mask-based rejection
//! sampling so every symbol is equally likely.

use crate::{rng, Error};

/// Default 64-symbol URL-safe alphabet (`A-Za-z0-9_-`).
pub const URL_ALPHABET: &str = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict";
/// Default size, giving a collision probability similar to UUID v4.
pub const DEFAULT_SIZE: usize = 21;

/// A default-size ID using [`URL_ALPHABET`].
pub fn nanoid() -> String {
    let mut buf = [0u8; DEFAULT_SIZE];
    rng::fill(&mut buf);
    url_encode(&mut buf);
    // SAFETY: url_encode only writes ASCII bytes.
    unsafe { String::from_utf8_unchecked(buf.to_vec()) }
}

/// An ID of `size` symbols using [`URL_ALPHABET`].
pub fn nanoid_with_size(size: usize) -> Result<String, Error> {
    if size == 0 {
        return Err(Error::Size);
    }
    let mut buf = vec![0u8; size];
    rng::fill(&mut buf);
    url_encode(&mut buf);
    // SAFETY: url_encode only writes ASCII bytes.
    Ok(unsafe { String::from_utf8_unchecked(buf) })
}

fn url_encode(buf: &mut [u8]) {
    let alphabet = URL_ALPHABET.as_bytes();
    for b in buf.iter_mut() {
        *b = alphabet[(*b & 63) as usize];
    }
}

/// A generator bound to a custom alphabet and default size.
#[derive(Debug, Clone)]
pub struct CustomAlphabet {
    symbols: Vec<char>,
    ascii: bool,
    mask: u8,
    size: usize,
}

impl CustomAlphabet {
    pub fn new(alphabet: &str, size: usize) -> Result<Self, Error> {
        let symbols: Vec<char> = alphabet.chars().collect();
        let len = symbols.len();
        if !(1..=256).contains(&len) {
            return Err(Error::Alphabet);
        }
        if size == 0 {
            return Err(Error::Size);
        }
        let bits = u32::BITS - ((len as u32 - 1) | 1).leading_zeros();
        let mask = ((2u32 << (bits - 1)) - 1) as u8;
        Ok(CustomAlphabet { ascii: alphabet.is_ascii(), symbols, mask, size })
    }

    /// Generate an ID of the default size from the OS CSPRNG.
    pub fn generate(&self) -> String {
        self.generate_with(self.size, rng::fill)
    }

    /// Generate an ID of `size` symbols from the OS CSPRNG.
    pub fn generate_size(&self, size: usize) -> Result<String, Error> {
        if size == 0 {
            return Err(Error::Size);
        }
        Ok(self.generate_with(size, rng::fill))
    }

    /// Generate using a caller-supplied random source (nanoid's `customRandom`).
    pub fn generate_with<F: FnMut(&mut [u8])>(&self, size: usize, mut random: F) -> String {
        let len = self.symbols.len();
        let mask = self.mask as usize;
        let step = if mask + 1 == len { size } else { (8 * mask * size).div_ceil(5 * len) };
        let mut buf = vec![0u8; step];
        let mut out = String::with_capacity(if self.ascii { size } else { size * 4 });
        let mut count = 0;
        loop {
            random(&mut buf);
            for &b in &buf {
                let i = b as usize & mask;
                if i < len {
                    out.push(self.symbols[i]);
                    count += 1;
                    if count == size {
                        return out;
                    }
                }
            }
        }
    }
}
