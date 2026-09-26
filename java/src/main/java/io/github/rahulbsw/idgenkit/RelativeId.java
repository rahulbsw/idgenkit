package io.github.rahulbsw.idgenkit;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReferenceArray;
import java.util.function.Consumer;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Relative IDs: sortable, unique IDs in which every ID generated for the same key starts with the
 * same tag.
 *
 * <pre>
 * 3F8KQZ-01J8Y4Q9M3-X3B0N6E3P8
 * tag    48-bit ms  50-bit random / counter
 * </pre>
 *
 * <p>The tag is the top 30 bits of {@code HMAC-SHA-256(secret, BE32(len(salt)) || salt || key)}.
 * The secret keeps tags unguessable and must come from configuration or a key store, never from
 * source code. The optional salt gives the same key unrelated tags in different contexts.
 *
 * <p>{@link #generate} draws 50 fresh random bits per ID. {@link #monotonic} shares one counter
 * across all keys, so each key's IDs strictly increase and no two IDs from one instance are equal.
 * Instances are thread-safe.
 */
public final class RelativeId {
    public static final int MIN_SECRET_BYTES = 16;
    public static final int MAX_TAG = (1 << 30) - 1;
    public static final long MAX_TIMESTAMP = (1L << 48) - 1;
    public static final long MAX_RANDOM = (1L << 50) - 1;

    private static final char[] ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ".toCharArray();
    private static final byte[] DECODE = new byte[128];

    static {
        Arrays.fill(DECODE, (byte) -1);
        for (int i = 0; i < ALPHABET.length; i++) {
            DECODE[ALPHABET[i]] = (byte) i;
            DECODE[Character.toLowerCase(ALPHABET[i])] = (byte) i;
        }
    }

    /** A decoded relative ID. */
    public record Parts(int tag, long timestampMs, long random) {
        public Parts {
            if (tag < 0 || tag > MAX_TAG) {
                throw new IllegalArgumentException("tag must fit in 30 bits: " + tag);
            }
            if (timestampMs < 0 || timestampMs > MAX_TIMESTAMP) {
                throw new IllegalArgumentException("timestamp must fit in 48 bits: " + timestampMs);
            }
            if (random < 0 || random > MAX_RANDOM) {
                throw new IllegalArgumentException("random must fit in 50 bits: " + random);
            }
        }

        /** The 6-character tag every ID with this tag starts with. */
        public String tagText() {
            return encodeTag(tag);
        }

        /** The canonical 28-character form. */
        @Override
        public String toString() {
            char[] c = new char[28];
            put(c, 0, 6, tag);
            c[6] = '-';
            put(c, 7, 10, timestampMs);
            c[17] = '-';
            put(c, 18, 10, random);
            return new String(c);
        }
    }

    // Tags of recently used keys up to CACHE_MAX_KEY chars, in a direct-mapped table.
    static final int CACHE_SLOTS = 256;
    static final int CACHE_MAX_KEY = 64;

    private record CachedTag(String key, int tag) {}

    private final Mac prototype;
    private final AtomicReferenceArray<CachedTag> cache = new AtomicReferenceArray<>(CACHE_SLOTS);
    private long lastMs = -1;
    private long lastRand;

    public RelativeId(byte[] secret) {
        this(secret, "");
    }

    /**
     * @throws IllegalArgumentException if the secret is shorter than {@link #MIN_SECRET_BYTES}
     */
    public RelativeId(byte[] secret, String salt) {
        if (secret == null || secret.length < MIN_SECRET_BYTES) {
            throw new IllegalArgumentException("secret must be at least " + MIN_SECRET_BYTES + " bytes");
        }
        byte[] s = salt.getBytes(StandardCharsets.UTF_8);
        try {
            prototype = Mac.getInstance("HmacSHA256");
            prototype.init(new SecretKeySpec(secret, "HmacSHA256"));
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("HmacSHA256 unavailable", e);
        }
        prototype.update(new byte[] {(byte) (s.length >>> 24), (byte) (s.length >>> 16),
                (byte) (s.length >>> 8), (byte) s.length});
        prototype.update(s);
    }

    /** The 30-bit tag for {@code key}. */
    public int tagValue(String key) {
        if (key.length() > CACHE_MAX_KEY) {
            return computeTag(key);
        }
        int h = key.hashCode();
        int slot = (h ^ h >>> 16) & (CACHE_SLOTS - 1);
        CachedTag c = cache.get(slot);
        if (c != null && c.key.equals(key)) {
            return c.tag;
        }
        int tag = computeTag(key);
        cache.set(slot, new CachedTag(key, tag));
        return tag;
    }

    int computeTag(String key) {
        Mac mac;
        try {
            mac = (Mac) prototype.clone();
        } catch (CloneNotSupportedException e) {
            throw new IllegalStateException("HmacSHA256 provider does not support clone", e);
        }
        byte[] m = mac.doFinal(key.getBytes(StandardCharsets.UTF_8));
        return ((m[0] & 0xFF) << 24 | (m[1] & 0xFF) << 16 | (m[2] & 0xFF) << 8 | (m[3] & 0xFF)) >>> 2;
    }

    /** The 6-character tag that starts every ID for {@code key}. */
    public String tag(String key) {
        return encodeTag(tagValue(key));
    }

    /** A new ID for {@code key} with 50 fresh random bits. */
    public String generate(String key) {
        byte[] r = new byte[7];
        Rng.fill(r);
        return new Parts(tagValue(key), System.currentTimeMillis(), random50(r)).toString();
    }

    /**
     * A new ID for {@code key} from the counter shared by every key.
     *
     * @throws IllegalStateException if the counter is exhausted within one millisecond
     */
    public String monotonic(String key) {
        return monotonic(tagValue(key), System.currentTimeMillis(), Rng::fill);
    }

    synchronized String monotonic(int tag, long now, Consumer<byte[]> fill) {
        if (lastMs >= 0 && now <= lastMs) {
            if (lastRand == MAX_RANDOM) {
                throw new IllegalStateException("relative ID counter overflow within one millisecond");
            }
            lastRand++;
        } else {
            if (now < 0 || now > MAX_TIMESTAMP) {
                throw new IllegalArgumentException("timestamp out of range: " + now);
            }
            byte[] r = new byte[7];
            fill.accept(r);
            lastMs = now;
            lastRand = random50(r);
        }
        return new Parts(tag, lastMs, lastRand).toString();
    }

    public static String fromParts(int tag, long timestampMs, long random) {
        return new Parts(tag, timestampMs, random).toString();
    }

    /** The 6-character text form of a 30-bit tag. */
    public static String encodeTag(int tag) {
        if (tag < 0 || tag > MAX_TAG) {
            throw new IllegalArgumentException("tag must fit in 30 bits: " + tag);
        }
        char[] c = new char[6];
        put(c, 0, 6, tag);
        return new String(c);
    }

    /**
     * Parses the case-insensitive 28-character form, or the same 26 characters without hyphens.
     *
     * @throws IllegalArgumentException for anything else
     */
    public static Parts parse(CharSequence s) {
        int len = s.length();
        int sep;
        if (len == 28 && s.charAt(6) == '-' && s.charAt(17) == '-') {
            sep = 1;
        } else if (len == 26) {
            sep = 0;
        } else {
            throw new IllegalArgumentException("relative ID must be 28 characters (or 26 without hyphens): " + s);
        }
        long tag = get(s, 0, 6);
        long ms = get(s, 6 + sep, 10);
        long rand = get(s, 16 + 2 * sep, 10);
        if (tag < 0 || ms < 0 || rand < 0 || ms > MAX_TIMESTAMP) {
            throw new IllegalArgumentException("invalid relative ID: " + s);
        }
        return new Parts((int) tag, ms, rand);
    }

    private static long random50(byte[] r) {
        long v = 0;
        for (byte b : r) {
            v = v << 8 | (b & 0xFF);
        }
        return v & MAX_RANDOM;
    }

    private static void put(char[] c, int off, int width, long v) {
        for (int i = off + width - 1; i >= off; i--) {
            c[i] = ALPHABET[(int) (v & 31)];
            v >>>= 5;
        }
    }

    private static long get(CharSequence s, int off, int width) {
        long v = 0;
        for (int i = off; i < off + width; i++) {
            char ch = s.charAt(i);
            int d = ch < 128 ? DECODE[ch] : -1;
            if (d < 0) {
                return -1;
            }
            v = v << 5 | d;
        }
        return v;
    }
}
