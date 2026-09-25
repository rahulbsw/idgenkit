package io.github.rahulbsw.idgenkit;

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
    public synchronized Ulid next() {
        long now = System.currentTimeMillis();
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
        Rng.fill(r);
        last = Ulid.of(now, r);
        return last;
    }

    synchronized void seed(Ulid value) {
        last = value;
    }
}
