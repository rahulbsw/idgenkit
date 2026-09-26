//! SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104) for the relative ID tag;
//! the standard library has neither.

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const IV: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

#[derive(Clone)]
pub(crate) struct Sha256 {
    h: [u32; 8],
    total: u64,
    buf: [u8; 64],
    used: usize,
}

fn block(h: &mut [u32; 8], p: &[u8]) {
    let mut w = [0u32; 64];
    for (i, c) in p.chunks_exact(4).enumerate() {
        w[i] = u32::from_be_bytes([c[0], c[1], c[2], c[3]]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16]
            .wrapping_add(s0)
            .wrapping_add(w[i - 7])
            .wrapping_add(s1);
    }
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = *h;
    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let t1 = hh
            .wrapping_add(s1)
            .wrapping_add((e & f) ^ (!e & g))
            .wrapping_add(K[i])
            .wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let t2 = s0.wrapping_add((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d.wrapping_add(t1);
        d = c;
        c = b;
        b = a;
        a = t1.wrapping_add(t2);
    }
    for (x, v) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
        *x = x.wrapping_add(v);
    }
}

impl Sha256 {
    pub(crate) fn new() -> Sha256 {
        Sha256 {
            h: IV,
            total: 0,
            buf: [0; 64],
            used: 0,
        }
    }

    pub(crate) fn update(&mut self, mut p: &[u8]) {
        self.total = self.total.wrapping_add(p.len() as u64);
        if self.used > 0 {
            let take = (64 - self.used).min(p.len());
            self.buf[self.used..self.used + take].copy_from_slice(&p[..take]);
            self.used += take;
            p = &p[take..];
            if self.used < 64 {
                return;
            }
            let buf = self.buf;
            block(&mut self.h, &buf);
            self.used = 0;
        }
        let mut chunks = p.chunks_exact(64);
        for c in &mut chunks {
            block(&mut self.h, c);
        }
        let rest = chunks.remainder();
        self.buf[..rest.len()].copy_from_slice(rest);
        self.used = rest.len();
    }

    pub(crate) fn finish(mut self) -> [u8; 32] {
        let bits = self.total.wrapping_mul(8);
        let mut pad = [0u8; 72];
        pad[0] = 0x80;
        let n = if self.used < 56 { 56 } else { 120 } - self.used;
        pad[n..n + 8].copy_from_slice(&bits.to_be_bytes());
        self.update(&pad[..n + 8]);
        let mut out = [0u8; 32];
        for (o, h) in out.chunks_exact_mut(4).zip(self.h) {
            o.copy_from_slice(&h.to_be_bytes());
        }
        out
    }

    pub(crate) fn wipe(&mut self) {
        let p = self as *mut Sha256 as *mut u8;
        for i in 0..std::mem::size_of::<Sha256>() {
            // SAFETY: p points to size_of::<Sha256>() bytes owned by self; every
            // field is plain integers, so any byte pattern is valid.
            unsafe { std::ptr::write_volatile(p.add(i), 0) };
        }
    }
}

/// HMAC-SHA-256 with the key already absorbed into the inner and outer states.
#[derive(Clone)]
pub(crate) struct Hmac {
    pub(crate) inner: Sha256,
    outer: Sha256,
}

impl Hmac {
    pub(crate) fn new(key: &[u8]) -> Hmac {
        let mut k = [0u8; 64];
        if key.len() > 64 {
            let mut s = Sha256::new();
            s.update(key);
            k[..32].copy_from_slice(&s.finish());
        } else {
            k[..key.len()].copy_from_slice(key);
        }
        let mut inner = Sha256::new();
        let mut outer = Sha256::new();
        inner.update(&k.map(|b| b ^ 0x36));
        outer.update(&k.map(|b| b ^ 0x5c));
        for b in k.iter_mut() {
            // SAFETY: b is a valid, aligned u8 inside k.
            unsafe { std::ptr::write_volatile(b, 0) };
        }
        Hmac { inner, outer }
    }

    /// Finishes a copy of the prepared state with `msg`; `self` stays reusable.
    pub(crate) fn mac(&self, msg: &[u8]) -> [u8; 32] {
        let mut inner = self.inner.clone();
        inner.update(msg);
        let mut outer = self.outer.clone();
        outer.update(&inner.finish());
        outer.finish()
    }

    pub(crate) fn wipe(&mut self) {
        self.inner.wipe();
        self.outer.wipe();
    }
}

#[cfg(test)]
mod tests {
    use super::Hmac;

    fn unhex(s: &str) -> Vec<u8> {
        if s == "-" {
            return Vec::new();
        }
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn hmac_vectors() {
        let rows = crate::test_vectors("hmac_sha256.txt");
        assert!(rows.len() > 20);
        for r in rows {
            let got = Hmac::new(&unhex(&r[0])).mac(&unhex(&r[1]));
            assert_eq!(got.to_vec(), unhex(&r[2]), "{r:?}");
        }
    }
}
