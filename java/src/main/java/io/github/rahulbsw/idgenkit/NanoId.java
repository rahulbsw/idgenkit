package io.github.rahulbsw.idgenkit;

import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/**
 * Compact, URL-friendly random string IDs following https://github.com/ai/nanoid.
 *
 * <p>Custom alphabets use mask-based rejection sampling so every symbol is equally likely.
 * Alphabets are sequences of Unicode code points (1 to 256 of them).
 */
public final class NanoId {
    public static final String URL_ALPHABET =
            "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict";
    public static final int DEFAULT_SIZE = 21;

    private static final byte[] URL_BYTES = URL_ALPHABET.getBytes(StandardCharsets.US_ASCII);

    private final int[] symbols;
    private final int mask;
    private final int size;
    private final Consumer<byte[]> random;

    private NanoId(String alphabet, int size, Consumer<byte[]> random) {
        this.symbols = alphabet.codePoints().toArray();
        if (symbols.length < 1 || symbols.length > 256) {
            throw new IllegalArgumentException("alphabet must contain between 1 and 256 symbols");
        }
        checkSize(size);
        this.size = size;
        this.random = random;
        int bits = 32 - Integer.numberOfLeadingZeros((symbols.length - 1) | 1);
        this.mask = (2 << (bits - 1)) - 1;
    }

    /** A 21-character ID using {@link #URL_ALPHABET}. */
    public static String generate() {
        return generate(DEFAULT_SIZE);
    }

    /** An ID of {@code size} characters using {@link #URL_ALPHABET}. */
    public static String generate(int size) {
        checkSize(size);
        byte[] b = new byte[size];
        Rng.fill(b);
        for (int i = 0; i < size; i++) {
            b[i] = URL_BYTES[b[i] & 63];
        }
        return new String(b, StandardCharsets.ISO_8859_1);
    }

    /** A reusable, thread-safe generator for a custom alphabet. */
    public static NanoId customAlphabet(String alphabet, int size) {
        return new NanoId(alphabet, size, Rng::fill);
    }

    /** Like {@link #customAlphabet} but with a caller-supplied random byte source. */
    public static NanoId customRandom(String alphabet, int size, Consumer<byte[]> random) {
        return new NanoId(alphabet, size, random);
    }

    public String next() {
        return next(size);
    }

    public String next(int size) {
        checkSize(size);
        int len = symbols.length;
        int step = mask + 1 == len ? size : (int) ((8L * mask * size + 5L * len - 1) / (5L * len));
        byte[] buf = new byte[step];
        StringBuilder out = new StringBuilder(size);
        int count = 0;
        while (true) {
            random.accept(buf);
            for (byte b : buf) {
                int i = b & mask;
                if (i < len) {
                    out.appendCodePoint(symbols[i]);
                    if (++count == size) {
                        return out.toString();
                    }
                }
            }
        }
    }

    private static void checkSize(int size) {
        if (size < 1) {
            throw new IllegalArgumentException("size must be >= 1");
        }
    }
}
