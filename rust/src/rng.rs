//! OS cryptographic randomness without third-party crates.

#[cfg(any(
    target_os = "macos",
    target_os = "ios",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd",
    target_os = "dragonfly"
))]
mod sys {
    extern "C" {
        // Userspace ChaCha20 CSPRNG seeded by the kernel; fork-safe, never fails.
        fn arc4random_buf(buf: *mut u8, len: usize);
    }

    pub fn fill(buf: &mut [u8]) -> std::io::Result<()> {
        unsafe { arc4random_buf(buf.as_mut_ptr(), buf.len()) };
        Ok(())
    }
}

#[cfg(all(
    unix,
    not(any(
        target_os = "macos",
        target_os = "ios",
        target_os = "freebsd",
        target_os = "openbsd",
        target_os = "netbsd",
        target_os = "dragonfly"
    ))
))]
mod sys {
    extern "C" {
        // Available on macOS 10.12+, glibc 2.25+, musl, and the BSDs.
        fn getentropy(buf: *mut u8, len: usize) -> i32;
    }

    pub fn fill(buf: &mut [u8]) -> std::io::Result<()> {
        // getentropy accepts at most 256 bytes per call.
        for chunk in buf.chunks_mut(256) {
            if unsafe { getentropy(chunk.as_mut_ptr(), chunk.len()) } != 0 {
                return Err(std::io::Error::last_os_error());
            }
        }
        Ok(())
    }
}

#[cfg(windows)]
mod sys {
    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 0x0000_0002;

    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(alg: *mut core::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
    }

    pub fn fill(buf: &mut [u8]) -> std::io::Result<()> {
        for chunk in buf.chunks_mut(u32::MAX as usize) {
            let status = unsafe {
                BCryptGenRandom(
                    core::ptr::null_mut(),
                    chunk.as_mut_ptr(),
                    chunk.len() as u32,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG,
                )
            };
            if status != 0 {
                return Err(std::io::Error::other(format!("BCryptGenRandom failed: {status:#x}")));
            }
        }
        Ok(())
    }
}

/// Fill `buf` from the operating system CSPRNG.
///
/// # Panics
/// If the OS random source fails, which does not happen on supported systems.
pub fn fill(buf: &mut [u8]) {
    if let Err(e) = sys::fill(buf) {
        panic!("idgenkit: OS random source failed: {e}");
    }
}
