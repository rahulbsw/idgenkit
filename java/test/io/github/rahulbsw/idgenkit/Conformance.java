package io.github.rahulbsw.idgenkit;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongSupplier;

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
        run("ulidMonotonicVectors", Conformance::ulidMonotonicVectors);
        run("uuidVectors", Conformance::uuidVectors);
        run("uuidInvalid", Conformance::uuidInvalid);
        run("uuidGenerate", Conformance::uuidGenerate);
        run("uuidMonotonic", Conformance::uuidMonotonic);
        run("uuidMonotonicVectors", Conformance::uuidMonotonicVectors);
        run("relidTagVectors", Conformance::relidTagVectors);
        run("relidVectors", Conformance::relidVectors);
        run("relidInvalid", Conformance::relidInvalid);
        run("relidMonotonicVectors", Conformance::relidMonotonicVectors);
        run("relidGenerate", Conformance::relidGenerate);
        run("relidMonotonic", Conformance::relidMonotonic);
        run("relidTagCache", Conformance::relidTagCache);
        run("snowflakeVectors", Conformance::snowflakeVectors);
        run("snowflakeSequenceVectors", Conformance::snowflakeSequenceVectors);
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

    static void ulidMonotonicVectors() throws IOException {
        List<String[]> rows = vectors("ulid_monotonic.txt");
        check(rows.size() > 10, "too few ulid_monotonic vectors");
        MonotonicUlid g = new MonotonicUlid();
        for (String[] r : rows) {
            if (r[0].equals("reset")) {
                g = new MonotonicUlid();
                continue;
            }
            String where = String.join(" ", r);
            boolean[] drew = {false};
            MonotonicUlid gen = g;
            Runnable step = () -> {
                Ulid u = gen.next(Long.parseLong(r[1]), buf -> {
                    drew[0] = true;
                    if (!r[2].equals("-")) {
                        System.arraycopy(hex(r[2]), 0, buf, 0, buf.length);
                    }
                });
                check(u.toString().equals(r[3]), where + ": got " + u);
            };
            if (r[3].equals("error")) {
                throwsIllegal(step, where);
            } else {
                step.run();
            }
            check(!(r[2].equals("-") && drew[0]), where + ": drew randomness");
        }
    }

    static void uuidVectors() throws IOException {
        List<String[]> rows = vectors("uuid.txt");
        check(rows.size() > 10, "too few uuid vectors");
        for (String[] r : rows) {
            String where = String.join(" ", r);
            String want = r[r.length - 1];
            boolean v7 = r[0].equals("v7");
            if (want.equals("error")) {
                throwsIllegal(() -> Uuids.v7(Long.parseLong(r[1]), hex(r[2])), where);
                continue;
            }
            UUID u = v7 ? Uuids.v7(Long.parseLong(r[1]), hex(r[2])) : Uuids.v4(hex(r[1]));
            check(u.toString().equals(want), where + ": got " + u);
            check(Uuids.parse(want).equals(u) && Uuids.parse(want.toUpperCase()).equals(u), "parse " + want);
            check(u.version() == (v7 ? 7 : 4) && u.variant() == 2, "version/variant " + want);
            if (v7) {
                check(Uuids.timestamp(u) == Long.parseLong(r[1]), "timestamp " + want);
            } else {
                throwsIllegal(() -> Uuids.timestamp(u), "v4 timestamp " + want);
            }
        }
    }

    static void uuidInvalid() throws IOException {
        List<String> rows = new ArrayList<>();
        for (String line : Files.readAllLines(testdata.resolve("uuid_invalid.txt"))) {
            if (!line.startsWith("#")) {
                rows.add(line.substring(1, line.length() - 1));
            }
        }
        check(rows.size() > 10, "too few uuid_invalid vectors");
        for (String s : rows) {
            throwsIllegal(() -> Uuids.parse(s), '"' + s + '"');
        }
        throwsIllegal(() -> Uuids.parse("1-1-1-1-1"), "UUID.fromString short form");
        throwsIllegal(() -> Uuids.parse("017f22e2-79b0-7cc3-98c4-dc0c0c07398\uFF10"), "fullwidth digit");
    }

    static void uuidGenerate() {
        Set<UUID> set = new HashSet<>();
        long now = System.currentTimeMillis();
        for (int i = 0; i < 10_000; i++) {
            UUID a = Uuids.v4();
            UUID b = Uuids.v7();
            check(a.version() == 4 && b.version() == 7 && a.variant() == 2 && b.variant() == 2, "version " + b);
            set.add(a);
            set.add(b);
        }
        check(set.size() == 20_000, "duplicates");
        long ts = Uuids.timestamp(Uuids.v7());
        check(ts >= now && ts < now + 5000, "v7 timestamp " + ts);
    }

    static void uuidMonotonic() throws InterruptedException {
        MonotonicUuidV7 g = new MonotonicUuidV7();
        String prev = g.next().toString();
        for (int i = 0; i < 50_000; i++) {
            String s = g.next().toString();
            check(s.compareTo(prev) > 0, "not increasing: " + s + " after " + prev);
            prev = s;
        }
        Set<UUID> all = ConcurrentHashMap.newKeySet();
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

    static void uuidMonotonicVectors() throws IOException {
        List<String[]> rows = vectors("uuid7_monotonic.txt");
        check(rows.size() > 10, "too few uuid7_monotonic vectors");
        MonotonicUuidV7 g = new MonotonicUuidV7();
        for (String[] r : rows) {
            if (r[0].equals("reset")) {
                g = new MonotonicUuidV7();
                continue;
            }
            String where = String.join(" ", r);
            boolean[] drew = {false};
            MonotonicUuidV7 gen = g;
            Runnable step = () -> {
                UUID u = gen.next(Long.parseLong(r[1]), buf -> {
                    drew[0] = true;
                    if (!r[2].equals("-")) {
                        System.arraycopy(hex(r[2]), 0, buf, 0, buf.length);
                    }
                });
                check(u.toString().equals(r[3]), where + ": got " + u);
            };
            if (r[3].equals("error")) {
                throwsIllegal(step, where);
            } else {
                step.run();
            }
            check(!(r[2].equals("-") && drew[0]), where + ": drew randomness");
        }
    }

    private static final byte[] TEST_SECRET = "test-only-secret-0123456789".getBytes(StandardCharsets.UTF_8);

    private static String utf8OrEmpty(String hex) {
        return hex.equals("-") ? "" : new String(hex(hex), StandardCharsets.UTF_8);
    }

    static void relidTagVectors() throws IOException {
        List<String[]> rows = vectors("relid_tag.txt");
        check(rows.size() > 10, "too few relid_tag vectors");
        for (String[] r : rows) {
            RelativeId g = new RelativeId(hex(r[0]), utf8OrEmpty(r[1]));
            String key = utf8OrEmpty(r[2]);
            check(g.tagValue(key) == Integer.parseInt(r[3]), String.join(" ", r) + ": tag " + g.tagValue(key));
            check(g.tag(key).equals(r[4]), String.join(" ", r) + ": tag text " + g.tag(key));
        }
        throwsIllegal(() -> new RelativeId("0123456789abcde".getBytes(StandardCharsets.UTF_8)), "short secret");
    }

    static void relidVectors() throws IOException {
        List<String[]> rows = vectors("relid.txt");
        check(rows.size() > 10, "too few relid vectors");
        for (String[] r : rows) {
            String where = String.join(" ", r);
            if (r[0].equals("parts")) {
                long tag = Long.parseLong(r[1]);
                long ms = Long.parseLong(r[2]);
                long rand = Long.parseLong(r[3]);
                if (r[4].equals("error")) {
                    throwsIllegal(() -> RelativeId.fromParts((int) tag, ms, rand), where);
                    continue;
                }
                check(RelativeId.fromParts((int) tag, ms, rand).equals(r[4]), where);
                check(RelativeId.parse(r[4]).equals(new RelativeId.Parts((int) tag, ms, rand)), "parse " + where);
            } else {
                RelativeId.Parts p = RelativeId.parse(r[1]);
                check(p.tag() == Integer.parseInt(r[2]) && p.timestampMs() == Long.parseLong(r[3])
                        && p.random() == Long.parseLong(r[4]), "parse " + where);
            }
        }
    }

    static void relidInvalid() throws IOException {
        List<String> rows = new ArrayList<>();
        for (String line : Files.readAllLines(testdata.resolve("relid_invalid.txt"))) {
            if (!line.startsWith("#")) {
                rows.add(line.substring(1, line.length() - 1));
            }
        }
        check(rows.size() > 10, "too few relid_invalid vectors");
        for (String s : rows) {
            throwsIllegal(() -> RelativeId.parse(s), '"' + s + '"');
        }
    }

    static void relidMonotonicVectors() throws IOException {
        List<String[]> rows = vectors("relid_monotonic.txt");
        check(rows.size() > 10, "too few relid_monotonic vectors");
        RelativeId g = new RelativeId(TEST_SECRET);
        for (String[] r : rows) {
            if (r[0].equals("reset")) {
                g = new RelativeId(TEST_SECRET);
                continue;
            }
            String where = String.join(" ", r);
            boolean[] drew = {false};
            RelativeId gen = g;
            Runnable step = () -> {
                String id = gen.monotonic(Integer.parseInt(r[1]), Long.parseLong(r[2]), buf -> {
                    drew[0] = true;
                    if (!r[3].equals("-")) {
                        System.arraycopy(hex(r[3]), 0, buf, 0, buf.length);
                    }
                });
                check(id.equals(r[4]), where + ": got " + id);
            };
            if (r[4].equals("error")) {
                throwsIllegal(step, where);
            } else {
                step.run();
            }
            check(!(r[3].equals("-") && drew[0]), where + ": drew randomness");
        }
    }

    static void relidGenerate() {
        RelativeId orders = new RelativeId(TEST_SECRET, "orders");
        RelativeId invoices = new RelativeId(TEST_SECRET, "invoices");
        String tag = orders.tag("customer-42");
        long now = System.currentTimeMillis();
        Set<String> set = new HashSet<>();
        for (int i = 0; i < 10_000; i++) {
            String id = orders.generate("customer-42");
            check(id.startsWith(tag + "-"), "tag prefix " + id);
            set.add(id);
        }
        check(set.size() == 10_000, "duplicates");
        check(!invoices.tag("customer-42").equals(tag), "salt did not change the tag");
        RelativeId.Parts p = RelativeId.parse(orders.generate("customer-42"));
        check(p.tagText().equals(tag) && p.timestampMs() >= now && p.timestampMs() < now + 5000, "parts " + p);
    }

    static void relidMonotonic() throws InterruptedException {
        RelativeId g = new RelativeId(TEST_SECRET);
        String a = "";
        String b = "";
        for (int i = 0; i < 30_000; i++) {
            if (i % 3 == 0) {
                String id = g.monotonic("customer-7");
                check(id.compareTo(a) > 0, id + " after " + a);
                a = id;
            } else {
                String id = g.monotonic("customer-42");
                check(id.compareTo(b) > 0, id + " after " + b);
                b = id;
            }
        }
        Set<String> all = ConcurrentHashMap.newKeySet();
        Thread[] ts = new Thread[8];
        for (int t = 0; t < ts.length; t++) {
            ts[t] = new Thread(() -> {
                for (int i = 0; i < 5000; i++) {
                    all.add(g.monotonic("k"));
                }
            });
            ts[t].start();
        }
        for (Thread t : ts) {
            t.join();
        }
        check(all.size() == 40_000, "concurrent duplicates: " + all.size());
    }

    static void relidTagCache() throws InterruptedException {
        RelativeId g = new RelativeId(TEST_SECRET, "orders");
        List<String> keys = new ArrayList<>(List.of("", "k".repeat(RelativeId.CACHE_MAX_KEY),
                "k".repeat(RelativeId.CACHE_MAX_KEY + 1), "Aa", "BB")); // "Aa" and "BB" share a hashCode
        for (int i = 0; i < 4 * RelativeId.CACHE_SLOTS; i++) {
            keys.add("customer-" + i);
        }
        Set<String> bad = ConcurrentHashMap.newKeySet();
        Thread[] ts = new Thread[4];
        for (int t = 0; t < ts.length; t++) {
            ts[t] = new Thread(() -> {
                for (int round = 0; round < 3; round++) {
                    for (String k : keys) {
                        if (g.tagValue(k) != g.computeTag(k)) {
                            bad.add(k);
                        }
                    }
                }
            });
            ts[t].start();
        }
        for (Thread t : ts) {
            t.join();
        }
        check(bad.isEmpty(), "stale cached tags for " + bad);
    }

    static LongSupplier scriptedClock(String readings) {
        long[] values = Arrays.stream(readings.split(",")).mapToLong(Long::parseLong).toArray();
        int[] i = {0};
        return () -> {
            long v = values[i[0]];
            i[0] = Math.min(i[0] + 1, values.length - 1);
            return v;
        };
    }

    static void snowflakeSequenceVectors() throws IOException {
        List<String[]> rows = vectors("snowflake_sequence.txt");
        check(rows.size() > 10, "too few snowflake_sequence vectors");
        Snowflake g = null;
        long prev = 0;
        for (String[] r : rows) {
            String where = String.join(" ", r);
            switch (r[0]) {
                case "gen" -> {
                    g = new Snowflake(Integer.parseInt(r[1]), Long.parseLong(r[2]));
                    prev = 0;
                }
                case "fill" -> {
                    long clock = Long.parseLong(r[2]);
                    for (int n = Integer.parseInt(r[1]); n > 0; n--) {
                        long id = g.nextId(() -> clock);
                        check(Long.compareUnsigned(id, prev) > 0, where + ": " + id + " after " + prev);
                        prev = id;
                    }
                }
                case "next" -> {
                    Snowflake gen = g;
                    if (r[2].equals("error")) {
                        throwsIllegal(() -> gen.nextId(scriptedClock(r[1])), where);
                    } else {
                        prev = gen.nextId(scriptedClock(r[1]));
                        check(prev == Long.parseUnsignedLong(r[2]), where + ": got " + Long.toUnsignedString(prev));
                    }
                }
                default -> throw new AssertionError("bad line " + where);
            }
        }
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
