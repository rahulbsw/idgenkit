package io.github.rahulbsw.idgenkit;

import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;

/**
 * Lock-free generator of time-ordered 64-bit IDs.
 *
 * <p>Layout (compatible with https://github.com/dustinrouillard/snowflake-id):
 * {@code | 42 bits: ms since epoch | 10 bits: machine id | 12 bits: sequence |}.
 *
 * <p>IDs are unsigned 64-bit values held in a {@code long}; with epoch 0 they stay positive until
 * 2039. Use {@link Long#toUnsignedString(long)} for text and a recent custom epoch for signed
 * storage. After 4096 IDs in one millisecond the generator waits for the next millisecond; if the
 * wall clock moves backwards it keeps issuing from the last observed millisecond.
 */
public final class Snowflake {
    public static final int TIMESTAMP_BITS = 42;
    public static final int MACHINE_ID_BITS = 10;
    public static final int SEQUENCE_BITS = 12;
    public static final int MAX_MACHINE_ID = (1 << MACHINE_ID_BITS) - 1;
    public static final int MAX_SEQUENCE = (1 << SEQUENCE_BITS) - 1;
    public static final long MAX_TIMESTAMP_DELTA = (1L << TIMESTAMP_BITS) - 1;

    private static final int MACHINE_SHIFT = SEQUENCE_BITS;
    private static final int TIMESTAMP_SHIFT = SEQUENCE_BITS + MACHINE_ID_BITS;

    /** A decoded Snowflake ID. */
    public record Parts(long timestampMs, int machineId, int sequence) {}

    private final long machineId;
    private final long epochMs;
    /** {@code lastMs << SEQUENCE_BITS | sequence}, updated with a single CAS. */
    private final AtomicLong state = new AtomicLong();

    public Snowflake(int machineId, long epochMs) {
        if (machineId < 0 || machineId > MAX_MACHINE_ID) {
            throw new IllegalArgumentException("machineId must be in [0, 1023]");
        }
        if (epochMs < 0) {
            throw new IllegalArgumentException("epochMs must be non-negative");
        }
        this.machineId = machineId;
        this.epochMs = epochMs;
    }

    public Snowflake(int machineId) {
        this(machineId, 0);
    }

    public long nextId() {
        return nextId(System::currentTimeMillis);
    }

    long nextId(LongSupplier clock) {
        while (true) {
            long old = state.get();
            long lastMs = old >>> SEQUENCE_BITS;
            long seq = old & MAX_SEQUENCE;
            long now = clock.getAsLong();
            long ms;
            long next;
            if (now > lastMs) {
                ms = now;
                next = 0;
            } else if (seq < MAX_SEQUENCE) {
                ms = lastMs;
                next = seq + 1;
            } else {
                while (clock.getAsLong() <= lastMs) {
                    Thread.onSpinWait();
                }
                continue;
            }
            long delta = ms - epochMs;
            if (delta < 0 || delta > MAX_TIMESTAMP_DELTA) {
                throw new IllegalStateException("current time is outside the 42-bit range for this epoch");
            }
            if (state.compareAndSet(old, ms << SEQUENCE_BITS | next)) {
                return delta << TIMESTAMP_SHIFT | machineId << MACHINE_SHIFT | next;
            }
        }
    }

    public Parts parse(long id) {
        return parse(id, epochMs);
    }

    public static long compose(long timestampMs, int machineId, int sequence, long epochMs) {
        if (machineId < 0 || machineId > MAX_MACHINE_ID) {
            throw new IllegalArgumentException("machineId must be in [0, 1023]");
        }
        if (sequence < 0 || sequence > MAX_SEQUENCE) {
            throw new IllegalArgumentException("sequence must be in [0, 4095]");
        }
        long delta = timestampMs - epochMs;
        if (delta < 0 || delta > MAX_TIMESTAMP_DELTA) {
            throw new IllegalArgumentException("timestamp outside the 42-bit range for this epoch");
        }
        return delta << TIMESTAMP_SHIFT | (long) machineId << MACHINE_SHIFT | sequence;
    }

    public static Parts parse(long id, long epochMs) {
        return new Parts(
                (id >>> TIMESTAMP_SHIFT) + epochMs,
                (int) (id >>> MACHINE_SHIFT) & MAX_MACHINE_ID,
                (int) id & MAX_SEQUENCE);
    }

    void forceState(long lastMs, long sequence) {
        state.set(lastMs << SEQUENCE_BITS | sequence);
    }
}
