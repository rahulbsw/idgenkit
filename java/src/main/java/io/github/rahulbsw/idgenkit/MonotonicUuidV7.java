package io.github.rahulbsw.idgenkit;

import java.util.UUID;
import java.util.function.Consumer;

/**
 * Thread-safe generator of strictly increasing UUIDv7s.
 *
 * <p>Within the same millisecond (or if the clock moves backwards) the previous 74 random bits are
 * incremented by one instead of drawing fresh randomness. Compare the results by their string form
 * or bytes: {@link UUID#compareTo} uses signed arithmetic.
 */
public final class MonotonicUuidV7 {
    private static final long RAND_B_MASK = (1L << 62) - 1;

    private UUID last;

    /**
     * @throws IllegalStateException if 2^74 UUIDs are requested within one millisecond
     */
    public UUID next() {
        return next(System.currentTimeMillis(), Rng::fill);
    }

    synchronized UUID next(long now, Consumer<byte[]> fill) {
        UUID prev = last;
        if (prev != null && now <= Uuids.timestamp(prev)) {
            long msb = prev.getMostSignificantBits();
            long randA = msb & 0xFFF;
            long randB = prev.getLeastSignificantBits() & RAND_B_MASK;
            if (randB < RAND_B_MASK) {
                randB++;
            } else if (randA < 0xFFF) {
                randA++;
                randB = 0;
            } else {
                throw new IllegalStateException("UUIDv7 random component overflow within one millisecond");
            }
            last = new UUID((msb & ~0xFFFL) | randA, Long.MIN_VALUE | randB);
            return last;
        }
        if (now < 0 || now > Uuids.MAX_TIMESTAMP) {
            throw new IllegalArgumentException("timestamp out of range: " + now);
        }
        byte[] r = new byte[10];
        fill.accept(r);
        last = Uuids.v7(now, r);
        return last;
    }
}
