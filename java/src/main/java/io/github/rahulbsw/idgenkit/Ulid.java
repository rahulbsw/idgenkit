package io.github.rahulbsw.idgenkit;

import java.util.Arrays;
import java.util.UUID;

/**
 * Universally Unique Lexicographically Sortable Identifier.
 *
 * <p>128 bits: a 48-bit millisecond Unix timestamp followed by 80 bits of randomness, encoded as
 * 26 characters of Crockford base32. See https://github.com/ulid/spec. Natural ordering (unsigned)
 * matches both creation time and string order.
 */
public final class Ulid implements Comparable<Ulid> {
    public static final int ENCODED_LENGTH = 26;
    public static final long MAX_TIMESTAMP = (1L << 48) - 1;

    private static final char[] ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ".toCharArray();
    private static final byte[] DECODE = new byte[128];

    static {
        Arrays.fill(DECODE, (byte) -1);
        for (int i = 0; i < ALPHABET.length; i++) {
            DECODE[ALPHABET[i]] = (byte) i;
            DECODE[Character.toLowerCase(ALPHABET[i])] = (byte) i;
        }
    }

    private final long msb;
    private final long lsb;

    private Ulid(long msb, long lsb) {
        this.msb = msb;
        this.lsb = lsb;
    }

    /** A ULID for the current time with 80 bits of cryptographic randomness. */
    public static Ulid generate() {
        byte[] r = new byte[10];
        Rng.fill(r);
        return of(System.currentTimeMillis(), r);
    }

    /** Builds a ULID from a millisecond timestamp and exactly 10 random bytes. */
    public static Ulid of(long timestampMs, byte[] random) {
        if (timestampMs < 0 || timestampMs > MAX_TIMESTAMP) {
            throw new IllegalArgumentException("timestamp must fit in 48 bits");
        }
        if (random.length != 10) {
            throw new IllegalArgumentException("random component must be 10 bytes");
        }
        long msb = timestampMs << 16 | (random[0] & 0xFFL) << 8 | (random[1] & 0xFFL);
        long lsb = 0;
        for (int i = 2; i < 10; i++) {
            lsb = lsb << 8 | (random[i] & 0xFFL);
        }
        return new Ulid(msb, lsb);
    }

    public static Ulid fromBytes(byte[] bytes) {
        if (bytes.length != 16) {
            throw new IllegalArgumentException("ULID binary form must be 16 bytes");
        }
        long msb = 0;
        long lsb = 0;
        for (int i = 0; i < 8; i++) {
            msb = msb << 8 | (bytes[i] & 0xFFL);
            lsb = lsb << 8 | (bytes[i + 8] & 0xFFL);
        }
        return new Ulid(msb, lsb);
    }

    public static Ulid fromUuid(UUID uuid) {
        return new Ulid(uuid.getMostSignificantBits(), uuid.getLeastSignificantBits());
    }

    /** Parses a canonical, case-insensitive 26-character ULID. */
    public static Ulid parse(CharSequence s) {
        if (s.length() != ENCODED_LENGTH) {
            throw new IllegalArgumentException("ULID must be 26 characters: " + s);
        }
        long hi = 0;
        long lo = 0;
        for (int i = 0; i < ENCODED_LENGTH; i++) {
            char c = s.charAt(i);
            int v = c < 128 ? DECODE[c] : -1;
            if (v < 0) {
                throw new IllegalArgumentException("invalid ULID character in: " + s);
            }
            if (i == 0 && v > 7) {
                throw new IllegalArgumentException("ULID overflows 128 bits: " + s);
            }
            hi = hi << 5 | lo >>> 59;
            lo = lo << 5 | v;
        }
        return new Ulid(hi, lo);
    }

    public long timestamp() {
        return msb >>> 16;
    }

    /** The 80-bit random component as 10 big-endian bytes. */
    public byte[] random() {
        byte[] b = toBytes();
        return Arrays.copyOfRange(b, 6, 16);
    }

    public byte[] toBytes() {
        byte[] b = new byte[16];
        for (int i = 7; i >= 0; i--) {
            b[i] = (byte) (msb >>> (8 * (7 - i)));
            b[i + 8] = (byte) (lsb >>> (8 * (7 - i)));
        }
        return b;
    }

    public UUID toUuid() {
        return new UUID(msb, lsb);
    }

    long mostSignificantBits() {
        return msb;
    }

    long leastSignificantBits() {
        return lsb;
    }

    static Ulid ofBits(long msb, long lsb) {
        return new Ulid(msb, lsb);
    }

    @Override
    public String toString() {
        char[] out = new char[ENCODED_LENGTH];
        long hi = msb;
        long lo = lsb;
        for (int i = ENCODED_LENGTH - 1; i >= 0; i--) {
            out[i] = ALPHABET[(int) (lo & 31)];
            lo = lo >>> 5 | hi << 59;
            hi >>>= 5;
        }
        return new String(out);
    }

    @Override
    public int compareTo(Ulid o) {
        int c = Long.compareUnsigned(msb, o.msb);
        return c != 0 ? c : Long.compareUnsigned(lsb, o.lsb);
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Ulid && ((Ulid) o).msb == msb && ((Ulid) o).lsb == lsb;
    }

    @Override
    public int hashCode() {
        return Long.hashCode(msb) * 31 + Long.hashCode(lsb);
    }
}
