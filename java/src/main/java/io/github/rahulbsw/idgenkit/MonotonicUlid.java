package io.github.rahulbsw.idgenkit;

import java.util.function.Consumer;

/**
 * Thread-safe generator of strictly increasing ULIDs.
 *
 * <p>Within the same millisecond (or if the clock moves backwards) the previous random component
 * is incremented by one instead of drawing fresh randomness.
 */
public final class MonotonicUlid {
    private static final long RANDOM_HI_MASK = 0xFFFFL;

    private Ulid last;

    /**
     * @throws IllegalStateException if 2^80 ULIDs are requested within one millisecond
     */
    public Ulid next() {
        return next(System.currentTimeMillis(), Rng::fill);
    }

    synchronized Ulid next(long now, Consumer<byte[]> fill) {
        Ulid prev = last;
        if (prev != null && now <= prev.timestamp()) {
            long msb = prev.mostSignificantBits();
            long lsb = prev.leastSignificantBits() + 1;
            if (lsb == 0) {
                if ((msb & RANDOM_HI_MASK) == RANDOM_HI_MASK) {
                    throw new IllegalStateException("ULID random component overflow within one millisecond");
                }
                msb++;
            }
            last = Ulid.ofBits(msb, lsb);
            return last;
        }
        byte[] r = new byte[10];
        fill.accept(r);
        last = Ulid.of(now, r);
        return last;
    }

    synchronized void seed(Ulid value) {
        last = value;
    }
}
