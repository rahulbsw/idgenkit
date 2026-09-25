#include "idgenkit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *testdata = "../testdata";
static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static FILE *open_vectors(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", testdata, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    return f;
}

static void hex_decode(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

static void test_ulid_vectors(void) {
    FILE *f = open_vectors("ulid.txt");
    char line[256], hex[64], text[64];
    unsigned long long ts;
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%llu %63s %63s", &ts, hex, text) != 3)
            continue;
        uint8_t rnd[10];
        hex_decode(hex, rnd, 10);
        uid_ulid u, p;
        char enc[UID_ULID_LEN + 1] = {0};
        CHECK(uid_ulid_from_parts(&u, ts, rnd) == UID_OK, "from_parts %s", text);
        uid_ulid_encode(&u, enc);
        CHECK(strcmp(enc, text) == 0, "encode got %s want %s", enc, text);
        CHECK(uid_ulid_decode(text, strlen(text), &p) == UID_OK && memcmp(&p, &u, 16) == 0,
              "decode %s", text);
        CHECK(uid_ulid_timestamp(&p) == ts, "timestamp %s", text);
        for (char *c = text; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c = (char)(*c - 'A' + 'a');
        CHECK(uid_ulid_decode(text, strlen(text), &p) == UID_OK && memcmp(&p, &u, 16) == 0,
              "lowercase decode %s", text);
        rows++;
    }
    fclose(f);
    CHECK(rows > 5, "too few ulid vectors");
}

static void test_ulid_invalid(void) {
    FILE *f = open_vectors("ulid_invalid.txt");
    char line[256];
    uid_ulid u;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        CHECK(uid_ulid_decode(line, strlen(line), &u) != UID_OK, "accepted %s", line);
    }
    fclose(f);
    uint8_t rnd[10] = {0};
    CHECK(uid_ulid_from_parts(&u, UID_ULID_MAX_TIME + 1, rnd) == UID_ERR_RANGE, "time range");
}

static void test_ulid_monotonic(void) {
    uid_ulid_monotonic st = {0};
    uid_ulid prev, u;
    CHECK(uid_ulid_monotonic_next(&st, &prev) == UID_OK, "first");
    for (int i = 0; i < 100000; i++) {
        CHECK(uid_ulid_monotonic_next(&st, &u) == UID_OK, "next");
        CHECK(memcmp(&u, &prev, 16) > 0, "not increasing at %d", i);
        prev = u;
    }
    /* clock moved backwards: stay on last timestamp */
    uint64_t last_ts = uid_ulid_timestamp(&prev);
    CHECK(uid_ulid_monotonic_next_at(&st, last_ts - 1000, &u) == UID_OK, "backwards");
    CHECK(uid_ulid_timestamp(&u) == last_ts && memcmp(&u, &prev, 16) > 0, "backwards order");

    uid_ulid_monotonic ov = {0};
    uint8_t ff[10];
    memset(ff, 0xFF, sizeof ff);
    uid_ulid_from_parts(&ov.last, 1000, ff);
    ov.primed = 1;
    CHECK(uid_ulid_monotonic_next_at(&ov, 1000, &u) == UID_ERR_OVERFLOW, "overflow");
    CHECK(uid_ulid_monotonic_next_at(&ov, 1000, &u) == UID_ERR_OVERFLOW, "overflow persists");
    CHECK(uid_ulid_monotonic_next_at(&ov, 1001, &u) == UID_OK, "recovers next ms");
}

static void test_snowflake_vectors(void) {
    FILE *f = open_vectors("snowflake.txt");
    char line[256];
    unsigned long long epoch, ts, id;
    unsigned mid, seq;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%llu %u %u %llu %llu", &epoch, &mid, &seq, &ts, &id) != 5)
            continue;
        uint64_t got, pts;
        uint32_t pmid, pseq;
        CHECK(uid_snowflake_compose(ts, mid, seq, epoch, &got) == UID_OK && got == id, "compose %llu",
              id);
        uid_snowflake_parse(id, epoch, &pts, &pmid, &pseq);
        CHECK(pts == ts && pmid == mid && pseq == seq, "parse %llu", id);
    }
    fclose(f);
    uint64_t out;
    CHECK(uid_snowflake_compose(0, 1024, 0, 0, &out) == UID_ERR_RANGE, "machine range");
    CHECK(uid_snowflake_compose(0, 0, 4096, 0, &out) == UID_ERR_RANGE, "sequence range");
    CHECK(uid_snowflake_compose(5, 0, 0, 10, &out) == UID_ERR_RANGE, "before epoch");
}

static uint64_t shared_state;
#define SF_THREADS 8
#define SF_PER_THREAD 50000
static uint64_t sf_ids[SF_THREADS * SF_PER_THREAD];

static void *sf_worker(void *arg) {
    uint64_t *out = arg;
    for (int i = 0; i < SF_PER_THREAD; i++) {
        uint64_t id;
        if (uid_snowflake_next(&shared_state, 5, 0, &id) != UID_OK)
            return (void *)1;
        if (i > 0 && id <= out[i - 1])
            return (void *)2;
        out[i] = id;
    }
    return NULL;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void test_snowflake_concurrent(void) {
    pthread_t th[SF_THREADS];
    for (int t = 0; t < SF_THREADS; t++)
        pthread_create(&th[t], NULL, sf_worker, sf_ids + t * SF_PER_THREAD);
    for (int t = 0; t < SF_THREADS; t++) {
        void *rc;
        pthread_join(th[t], &rc);
        CHECK(rc == NULL, "worker %d failed (%p)", t, rc);
    }
    size_t n = SF_THREADS * SF_PER_THREAD;
    qsort(sf_ids, n, sizeof sf_ids[0], cmp_u64);
    size_t dups = 0;
    for (size_t i = 1; i < n; i++)
        dups += sf_ids[i] == sf_ids[i - 1];
    CHECK(dups == 0, "%zu duplicate snowflake ids", dups);

    uint64_t st = 0, id;
    CHECK(uid_snowflake_next(&st, 1, uid_now_ms() + 100000, &id) == UID_ERR_RANGE, "future epoch");
    CHECK(uid_snowflake_next(&st, 1024, 0, &id) == UID_ERR_RANGE, "machine id");
}

struct stream {
    const uint8_t *data;
    size_t len, pos;
};

static int stream_random(void *ctx, uint8_t *buf, size_t n) {
    struct stream *s = ctx;
    if (s->pos + n > s->len)
        return UID_ERR_RANDOM;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return UID_OK;
}

static void test_nanoid_vectors(void) {
    FILE *f = open_vectors("nanoid.txt");
    static char line[4096], alphabet[512], hex[2048], expected[512];
    int size;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%511s %d %2047s %511s", alphabet, &size, hex, expected) != 4)
            continue;
        uint8_t data[1024];
        size_t n = strlen(hex) / 2;
        hex_decode(hex, data, n);
        struct stream s = {data, n, 0};
        char out[512] = {0};
        CHECK(uid_nanoid_custom_random(out, (size_t)size, alphabet, strlen(alphabet), stream_random,
                                       &s) == UID_OK,
              "generate %s", alphabet);
        CHECK(strcmp(out, expected) == 0, "alphabet %s got %s want %s", alphabet, out, expected);
    }
    fclose(f);
}

static void test_nanoid_misc(void) {
    char out[2048];
    const char *a = uid_nanoid_url_alphabet;
    CHECK(uid_nanoid(out, 21, a, 64) == UID_OK, "default");
    for (int i = 0; i < 21; i++)
        CHECK(memchr(a, out[i], 64) != NULL, "symbol");
    CHECK(uid_nanoid(out, 2000, "0123456789", 10) == UID_OK, "large custom");
    CHECK(uid_nanoid(out, 0, a, 64) == UID_ERR_INVALID, "size 0");
    CHECK(uid_nanoid(out, 5, "", 0) == UID_ERR_INVALID, "empty alphabet");
    CHECK(uid_nanoid(out, 5, "a\xc3\xa9", 3) == UID_ERR_INVALID, "non-ascii alphabet");

    static char big[100000];
    int counts[10] = {0};
    CHECK(uid_nanoid(big, sizeof big, "abcdefghij", 10) == UID_OK, "distribution sample");
    for (size_t i = 0; i < sizeof big; i++)
        counts[big[i] - 'a']++;
    for (int i = 0; i < 10; i++)
        CHECK(counts[i] > 9500 && counts[i] < 10500, "skewed symbol %c: %d", 'a' + i, counts[i]);
}

int main(int argc, char **argv) {
    if (argc > 1)
        testdata = argv[1];
    test_ulid_vectors();
    test_ulid_invalid();
    test_ulid_monotonic();
    test_snowflake_vectors();
    test_snowflake_concurrent();
    test_nanoid_vectors();
    test_nanoid_misc();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("OK: C core conformance tests passed");
    return 0;
}
