package io.github.rahulbsw.idgenkit;

import java.util.UUID;

/**
 * RFC 9562 version 4 and version 7 UUIDs as {@link java.util.UUID} values.
 *
 * <p>Version 4 is the JDK's own {@link UUID#randomUUID()}. Version 7 is a 48-bit big-endian
 * millisecond Unix timestamp followed by 74 random bits. {@link UUID#toString()} already gives the
 * canonical lowercase form; {@link #parse} is a strict replacement for the lenient {@link
 * UUID#fromString}.
 */
public final class Uuids {
    /** Largest UUIDv7 timestamp in milliseconds. */
    public static final long MAX_TIMESTAMP = (1L << 48) - 1;

    private Uuids() {}

    /** Random (version 4) UUID; the same as {@link UUID#randomUUID()}. */
    public static UUID v4() {
        return UUID.randomUUID();
    }

    /** Sets the version and variant bits of 16 random bytes. */
    public static UUID v4(byte[] random) {
        if (random.length != 16) {
            throw new IllegalArgumentException("random must be 16 bytes");
        }
        return withVersion(random.clone(), 4);
    }

    /** Time-ordered (version 7) UUID for the current time. */
    public static UUID v7() {
        byte[] r = new byte[10];
        Rng.fill(r);
        return v7(System.currentTimeMillis(), r);
    }

    /**
     * Builds a UUIDv7; the version and variant bits overwrite 6 of the random bits.
     *
     * @throws IllegalArgumentException if the timestamp is outside [0, 2^48) or random is not 10 bytes
     */
    public static UUID v7(long timestampMs, byte[] random) {
        if (timestampMs < 0 || timestampMs > MAX_TIMESTAMP) {
            throw new IllegalArgumentException("timestamp out of range: " + timestampMs);
        }
        if (random.length != 10) {
            throw new IllegalArgumentException("random must be 10 bytes");
        }
        byte[] b = new byte[16];
        for (int i = 5; i >= 0; i--) {
            b[i] = (byte) (timestampMs >>> (8 * (5 - i)));
        }
        System.arraycopy(random, 0, b, 6, 10);
        return withVersion(b, 7);
    }

    /**
     * Unix timestamp in milliseconds of a version 7 UUID. ({@link UUID#timestamp()} only handles
     * version 1.)
     *
     * @throws IllegalArgumentException for other versions
     */
    public static long timestamp(UUID u) {
        if (u.version() != 7) {
            throw new IllegalArgumentException("not a version 7 UUID: " + u);
        }
        return u.getMostSignificantBits() >>> 16;
    }

    /**
     * Parses the case-insensitive 8-4-4-4-12 form. Braces, the {@code urn:uuid:} prefix, the
     * 32-digit form and the short forms {@link UUID#fromString} accepts are rejected.
     *
     * @throws IllegalArgumentException if the text is not a canonical UUID
     */
    public static UUID parse(String s) {
        if (s.length() != 36) {
            throw new IllegalArgumentException("UUID must be 36 characters: \"" + s + "\"");
        }
        long msb = 0;
        long lsb = 0;
        int digits = 0;
        for (int i = 0; i < 36; i++) {
            char c = s.charAt(i);
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (c != '-') {
                    throw new IllegalArgumentException("invalid UUID: \"" + s + "\"");
                }
                continue;
            }
            int d = Character.digit(c, 16);
            if (d < 0 || c > 'f') {
                throw new IllegalArgumentException("invalid UUID: \"" + s + "\"");
            }
            if (digits++ < 16) {
                msb = msb << 4 | d;
            } else {
                lsb = lsb << 4 | d;
            }
        }
        return new UUID(msb, lsb);
    }

    static UUID ofBytes(byte[] b) {
        long msb = 0;
        long lsb = 0;
        for (int i = 0; i < 8; i++) {
            msb = msb << 8 | (b[i] & 0xFF);
            lsb = lsb << 8 | (b[i + 8] & 0xFF);
        }
        return new UUID(msb, lsb);
    }

    private static UUID withVersion(byte[] b, int version) {
        b[6] = (byte) ((b[6] & 0x0F) | version << 4);
        b[8] = (byte) ((b[8] & 0x3F) | 0x80);
        return ofBytes(b);
    }
}
