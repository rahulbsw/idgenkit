package io.github.rahulbsw.idgenkit;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/** Dependency-free test runner: java Conformance <testdata-dir>. */
public final class Conformance {
    private static Path testdata;
    private static int passed;

    public static void main(String[] args) throws Exception {
        testdata = Path.of(args.length > 0 ? args[0] : "../testdata");
        run("ulidVectors", Conformance::ulidVectors);
        run("ulidInvalid", Conformance::ulidInvalid);
        run("ulidUnique", Conformance::ulidUnique);
        run("ulidMonotonic", Conformance::ulidMonotonic);
        run("ulidMonotonicOverflow", Conformance::ulidMonotonicOverflow);
        run("ulidMonotonicConcurrent", Conformance::ulidMonotonicConcurrent);
        run("snowflakeVectors", Conformance::snowflakeVectors);
        run("snowflakeValidation", Conformance::snowflakeValidation);
        run("snowflakeOrdered", Conformance::snowflakeOrdered);
        run("snowflakeClockBackwards", Conformance::snowflakeClockBackwards);
        run("snowflakeConcurrent", Conformance::snowflakeConcurrent);
        run("nanoidVectors", Conformance::nanoidVectors);
        run("nanoidDefault", Conformance::nanoidDefault);
        run("nanoidCustom", Conformance::nanoidCustom);
        run("nanoidDistribution", Conformance::nanoidDistribution);
        System.out.println("OK: " + passed + " tests passed");
    }

    interface Test {
        void run() throws Exception;
    }

    private static void run(String name, Test t) throws Exception {
        try {
            t.run();
            passed++;
            System.out.println("PASS " + name);
        } catch (Throwable e) {
            System.out.println("FAIL " + name);
            throw e;
        }
    }

    private static void check(boolean cond, String msg) {
        if (!cond) {
            throw new AssertionError(msg);
        }
    }

    private static void throwsIllegal(Runnable r, String msg) {
        try {
            r.run();
        } catch (IllegalArgumentException | IllegalStateException expected) {
            return;
        }
        throw new AssertionError("expected exception: " + msg);
    }

    private static List<String[]> vectors(String name) throws IOException {
        List<String[]> rows = new ArrayList<>();
        for (String line : Files.readAllLines(testdata.resolve(name))) {
            if (!line.isEmpty() && !line.startsWith("#")) {
                rows.add(line.trim().split("\\s+"));
            }
        }
        return rows;
    }

    private static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return b;
    }

    static void ulidVectors() throws IOException {
        for (String[] r : vectors("ulid.txt")) {
            long ts = Long.parseLong(r[0]);
            Ulid u = Ulid.of(ts, hex(r[1]));
            check(u.toString().equals(r[2]), "encode " + r[2] + " got " + u);
            Ulid p = Ulid.parse(r[2]);
            check(p.equals(u) && p.timestamp() == ts, "parse " + r[2]);
            check(Arrays.equals(p.random(), hex(r[1])), "random " + r[2]);
            check(Ulid.parse(r[2].toLowerCase()).equals(u), "lowercase " + r[2]);
            check(Ulid.fromBytes(u.toBytes()).equals(u), "bytes " + r[2]);
            check(Ulid.fromUuid(u.toUuid()).equals(u), "uuid " + r[2]);
        }
    }

    static void ulidInvalid() throws IOException {
        for (String[] r : vectors("ulid_invalid.txt")) {
            throwsIllegal(() -> Ulid.parse(r[0]), r[0]);
        }
        throwsIllegal(() -> Ulid.parse(""), "empty");
        throwsIllegal(() -> Ulid.of(Ulid.MAX_TIMESTAMP + 1, new byte[10]), "time range");
    }

    static void ulidUnique() {
        Set<Ulid> set = new HashSet<>();
        for (int i = 0; i < 10_000; i++) {
            set.add(Ulid.generate());
        }
        check(set.size() == 10_000, "duplicates");
    }

    static void ulidMonotonic() {
        MonotonicUlid g = new MonotonicUlid();
        Ulid prev = g.next();
        for (int i = 0; i < 50_000; i++) {
            Ulid u = g.next();
            check(u.compareTo(prev) > 0, "not increasing");
            check(u.toString().compareTo(prev.toString()) > 0, "string order");
            prev = u;
        }
    }

    static void ulidMonotonicOverflow() {
        MonotonicUlid g = new MonotonicUlid();
        byte[] r = new byte[10];
        Arrays.fill(r, (byte) 0xFF);
        r[0] = 0;
        g.seed(Ulid.of(Ulid.MAX_TIMESTAMP, r));
        Ulid carried = g.next();
        check(carried.random()[0] == 1 && carried.random()[9] == 0, "carry into high random bits");
        Arrays.fill(r, (byte) 0xFF);
        g.seed(Ulid.of(Ulid.MAX_TIMESTAMP, r));
        throwsIllegal(g::next, "overflow");
        throwsIllegal(g::next, "overflow persists");
    }

    static void ulidMonotonicConcurrent() throws InterruptedException {
        MonotonicUlid g = new MonotonicUlid();
        Set<Ulid> all = ConcurrentHashMap.newKeySet();
        Thread[] ts = new Thread[8];
        for (int t = 0; t < ts.length; t++) {
            ts[t] = new Thread(() -> {
                for (int i = 0; i < 5000; i++) {
                    all.add(g.next());
                }
            });
            ts[t].start();
        }
        for (Thread t : ts) {
            t.join();
        }
        check(all.size() == 40_000, "concurrent duplicates: " + all.size());
    }

    static void snowflakeVectors() throws IOException {
        for (String[] r : vectors("snowflake.txt")) {
            long epoch = Long.parseLong(r[0]);
            int mid = Integer.parseInt(r[1]);
            int seq = Integer.parseInt(r[2]);
            long ts = Long.parseLong(r[3]);
            long id = Long.parseUnsignedLong(r[4]);
            check(Snowflake.compose(ts, mid, seq, epoch) == id, "compose " + r[4]);
            check(Snowflake.parse(id, epoch).equals(new Snowflake.Parts(ts, mid, seq)), "parse " + r[4]);
        }
    }

    static void snowflakeValidation() {
        throwsIllegal(() -> new Snowflake(1024), "machine id");
        throwsIllegal(() -> Snowflake.compose(0, 0, 4096, 0), "sequence");
        throwsIllegal(() -> Snowflake.compose(5, 0, 0, 10), "time before epoch");
        throwsIllegal(() -> new Snowflake(1, System.currentTimeMillis() + 1_000_000).nextId(), "future epoch");
    }

    static void snowflakeOrdered() {
        Snowflake g = new Snowflake(7, 1_600_000_000_000L);
        long prev = g.nextId();
        for (int i = 0; i < 100_000; i++) {
            long id = g.nextId();
            check(id > prev, "not increasing");
            prev = id;
        }
        Snowflake.Parts p = g.parse(prev);
        check(p.machineId() == 7 && p.timestampMs() > 1_600_000_000_000L, "parts " + p);
    }

    static void snowflakeClockBackwards() {
        Snowflake g = new Snowflake(1);
        long first = g.nextId();
        long future = System.currentTimeMillis() + 5;
        g.forceState(future, 0);
        long second = g.nextId();
        check(second > first && g.parse(second).equals(new Snowflake.Parts(future, 1, 1)), "clock backwards");
    }

    static void snowflakeConcurrent() throws InterruptedException {
        Snowflake g = new Snowflake(3);
        Set<Long> all = ConcurrentHashMap.newKeySet();
        Thread[] ts = new Thread[8];
        for (int t = 0; t < ts.length; t++) {
            ts[t] = new Thread(() -> {
                for (int i = 0; i < 20_000; i++) {
                    all.add(g.nextId());
                }
            });
            ts[t].start();
        }
        for (Thread t : ts) {
            t.join();
        }
        check(all.size() == 160_000, "concurrent duplicates: " + all.size());
    }

    static void nanoidVectors() throws IOException {
        for (String[] r : vectors("nanoid.txt")) {
            byte[] stream = hex(r[2]);
            int[] pos = {0};
            NanoId g = NanoId.customRandom(r[0], Integer.parseInt(r[1]), buf -> {
                System.arraycopy(stream, pos[0], buf, 0, buf.length);
                pos[0] += buf.length;
            });
            String got = g.next();
            check(got.equals(r[3]), "alphabet " + r[0] + " got " + got + " want " + r[3]);
        }
    }

    static void nanoidDefault() {
        Set<String> set = new HashSet<>();
        for (int i = 0; i < 10_000; i++) {
            String id = NanoId.generate();
            check(id.length() == 21 && id.matches("[A-Za-z0-9_-]+"), "bad id " + id);
            set.add(id);
        }
        check(set.size() == 10_000, "duplicates");
        check(NanoId.generate(64).length() == 64, "size 64");
        throwsIllegal(() -> NanoId.generate(0), "size 0");
    }

    static void nanoidCustom() {
        String id = NanoId.customAlphabet("0123456789", 12).next();
        check(id.matches("[0-9]{12}"), "digits " + id);
        String u = NanoId.customAlphabet("αβγδ😀", 8).next();
        check(u.codePointCount(0, u.length()) == 8, "unicode length " + u);
        throwsIllegal(() -> NanoId.customAlphabet("", 5), "empty alphabet");
        throwsIllegal(() -> NanoId.customAlphabet("x".repeat(257), 5), "oversized alphabet");
    }

    static void nanoidDistribution() {
        String id = NanoId.customAlphabet("abcdefghij", 100_000).next();
        int[] counts = new int[10];
        for (int i = 0; i < id.length(); i++) {
            counts[id.charAt(i) - 'a']++;
        }
        for (int c : counts) {
            check(c >= 9_500 && c <= 10_500, "skewed count " + c);
        }
    }
}
