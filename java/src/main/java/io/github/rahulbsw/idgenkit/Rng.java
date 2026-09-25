package io.github.rahulbsw.idgenkit;

import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;

/** Per-thread SecureRandom instances, avoiding the global lock inside NativePRNG. */
final class Rng {
    private static final ThreadLocal<SecureRandom> RANDOM = ThreadLocal.withInitial(Rng::create);

    private Rng() {}

    private static SecureRandom create() {
        try {
            return SecureRandom.getInstance("DRBG");
        } catch (NoSuchAlgorithmException e) {
            return new SecureRandom();
        }
    }

    static void fill(byte[] bytes) {
        RANDOM.get().nextBytes(bytes);
    }
}
