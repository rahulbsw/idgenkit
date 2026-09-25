package io.github.rahulbsw.idgenkit;

import java.util.function.Supplier;

/**
 * Minimal JDK-only benchmark harness (no JMH): warm up so the JIT compiles the hot path, then
 * report the best of several timed batches. Results are indicative, not JMH-grade.
 */
public final class Bench {
    private static volatile Object sink;

    public static void main(String[] args) throws InterruptedException {
        int n = Integer.parseInt(System.getenv().getOrDefault("BENCH_N", "1000000"));
        int threads = Math.min(8, Runtime.getRuntime().availableProcessors());
        System.out.println("# Java " + System.getProperty("java.version") + ", N=" + n);

        MonotonicUlid mono = new MonotonicUlid();
        MonotonicUuidV7 uuidMono = new MonotonicUuidV7();
        Snowflake sf = new Snowflake(1);
        NanoId hex = NanoId.customAlphabet("0123456789abcdef", 21);
        String sample = Ulid.generate().toString();

        bench("ulid.generate", n, Ulid::generate);
        bench("ulid.generate.toString", n, () -> Ulid.generate().toString());
        bench("ulid.monotonic", n, mono::next);
        bench("ulid.parse", n, () -> Ulid.parse(sample));
        bench("uuid.v4", n, Uuids::v4);
        bench("uuid.v7", n, Uuids::v7);
        bench("uuid.v7.monotonic", n, uuidMono::next);
        bench("snowflake.nextId", n, sf::nextId);
        bench("nanoid(21)", n, () -> NanoId.generate());
        bench("nanoid.custom(hex,21)", n, hex::next);
        parallel("snowflake.nextId", n, threads, sf::nextId);
        parallel("ulid.generate", n, threads, Ulid::generate);
    }

    private static void bench(String name, int n, Supplier<?> f) {
        for (int i = 0; i < n; i++) {
            sink = f.get();
        }
        double best = Double.MAX_VALUE;
        for (int round = 0; round < 5; round++) {
            long start = System.nanoTime();
            for (int i = 0; i < n; i++) {
                sink = f.get();
            }
            best = Math.min(best, (System.nanoTime() - start) / (double) n);
        }
        report(name, best);
    }

    private static void parallel(String name, int n, int threads, Supplier<?> f) throws InterruptedException {
        int per = n / threads;
        Thread[] ts = new Thread[threads];
        long start = System.nanoTime();
        for (int t = 0; t < threads; t++) {
            ts[t] = new Thread(() -> {
                Object local = null;
                for (int i = 0; i < per; i++) {
                    local = f.get();
                }
                sink = local;
            });
            ts[t].start();
        }
        for (Thread t : ts) {
            t.join();
        }
        report(name + " x" + threads, (System.nanoTime() - start) / (double) (per * threads));
    }

    private static void report(String name, double ns) {
        System.out.printf("java    %-28s %10.1f ns/op %14.0f ops/s%n", name, ns, 1e9 / ns);
    }
}
