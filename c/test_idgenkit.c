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

struct fixed_random {
    uint8_t bytes[10];
    int present, calls;
};

static int fixed_random(void *ctx, uint8_t *buf, size_t n) {
    struct fixed_random *r = ctx;
    r->calls++;
    if (!r->present || n != sizeof r->bytes)
        return UID_ERR_RANDOM;
    memcpy(buf, r->bytes, n);
    return UID_OK;
}

static void test_ulid_monotonic_vectors(void) {
    FILE *f = open_vectors("ulid_monotonic.txt");
    char line[256], hex[32], want[32];
    unsigned long long now;
    uid_ulid_monotonic st = {0};
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (strncmp(line, "reset", 5) == 0) {
            memset(&st, 0, sizeof st);
            continue;
        }
        if (sscanf(line, "next %llu %31s %31s", &now, hex, want) != 3) {
            CHECK(0, "bad ulid_monotonic line: %s", line);
            continue;
        }
        struct fixed_random r = {{0}, strcmp(hex, "-") != 0, 0};
        if (r.present)
            hex_decode(hex, r.bytes, sizeof r.bytes);
        uid_ulid u;
        int rc = uid_ulid_monotonic_next_custom_random(&st, now, fixed_random, &r, &u);
        CHECK(r.present || r.calls == 0, "drew randomness at %llu", now);
        if (strcmp(want, "error") == 0) {
            CHECK(rc != UID_OK, "expected an error at %llu", now);
        } else {
            char enc[UID_ULID_LEN + 1] = {0};
            uid_ulid_encode(&u, enc);
            CHECK(rc == UID_OK && strcmp(enc, want) == 0, "at %llu got %s (rc %d) want %s", now,
                  enc, rc, want);
        }
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few ulid_monotonic vectors");
}

static void test_uuid_vectors(void) {
    FILE *f = open_vectors("uuid.txt");
    char line[256], hex[64], want[64];
    unsigned long long ms;
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        uid_uuid u, p;
        char enc[UID_UUID_LEN + 1] = {0};
        uint8_t rnd[16];
        int rc;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "v4 %63s %63s", hex, want) == 2) {
            hex_decode(hex, rnd, 16);
            uid_uuidv4_from_random(&u, rnd);
            rc = UID_OK;
        } else if (sscanf(line, "v7 %llu %63s %63s", &ms, hex, want) == 3) {
            hex_decode(hex, rnd, 10);
            rc = uid_uuidv7_from_parts(&u, ms, rnd);
        } else {
            CHECK(0, "bad uuid line: %s", line);
            continue;
        }
        rows++;
        if (strcmp(want, "error") == 0) {
            CHECK(rc == UID_ERR_RANGE, "expected a range error: %s", line);
            continue;
        }
        uid_uuid_encode(&u, enc);
        CHECK(rc == UID_OK && strcmp(enc, want) == 0, "got %s want %s", enc, want);
        CHECK(uid_uuid_decode(want, strlen(want), &p) == UID_OK && memcmp(&p, &u, 16) == 0,
              "decode %s", want);
        CHECK(uid_uuid_version(&p) == (unsigned)(line[1] - '0'), "version %s", want);
        uint64_t ts = 0;
        rc = uid_uuidv7_timestamp(&p, &ts);
        CHECK(line[1] == '7' ? rc == UID_OK && ts == ms : rc == UID_ERR_INVALID, "timestamp %s", want);
        for (char *c = want; *c; c++)
            if (*c >= 'a' && *c <= 'f')
                *c = (char)(*c - 'a' + 'A');
        CHECK(uid_uuid_decode(want, strlen(want), &p) == UID_OK && memcmp(&p, &u, 16) == 0,
              "uppercase decode %s", want);
    }
    fclose(f);
    CHECK(rows > 10, "too few uuid vectors");
}

static void test_uuid_invalid(void) {
    FILE *f = open_vectors("uuid_invalid.txt");
    char line[256];
    int rows = 0;
    uid_uuid u;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        size_t len = strlen(line);
        CHECK(len >= 2 && line[0] == '"' && line[len - 1] == '"', "unquoted line: %s", line);
        CHECK(uid_uuid_decode(line + 1, len - 2, &u) != UID_OK, "accepted %s", line);
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few uuid_invalid vectors");
}

static void test_uuid_generate(void) {
    uid_uuid a, b;
    uint64_t ts, now = uid_now_ms();
    CHECK(uid_uuidv4(&a) == UID_OK && uid_uuidv4(&b) == UID_OK && memcmp(&a, &b, 16) != 0, "v4 unique");
    CHECK(uid_uuid_version(&a) == 4 && (a.b[8] & 0xC0) == 0x80, "v4 version/variant");
    CHECK(uid_uuidv7(&a) == UID_OK && uid_uuid_version(&a) == 7 && (a.b[8] & 0xC0) == 0x80,
          "v7 version/variant");
    CHECK(uid_uuidv7_timestamp(&a, &ts) == UID_OK && ts >= now && ts < now + 5000, "v7 timestamp");
    uid_uuidv7_monotonic m = {0};
    CHECK(uid_uuidv7_monotonic_next(&m, &a) == UID_OK, "monotonic first");
    for (int i = 0; i < 100000; i++) {
        CHECK(uid_uuidv7_monotonic_next(&m, &b) == UID_OK && memcmp(&a, &b, 16) < 0, "monotonic order");
        a = b;
    }
}

static void test_uuidv7_monotonic_vectors(void) {
    FILE *f = open_vectors("uuid7_monotonic.txt");
    char line[256], hex[32], want[64];
    unsigned long long now;
    uid_uuidv7_monotonic st = {0};
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (strncmp(line, "reset", 5) == 0) {
            memset(&st, 0, sizeof st);
            continue;
        }
        if (sscanf(line, "next %llu %31s %63s", &now, hex, want) != 3) {
            CHECK(0, "bad uuid7_monotonic line: %s", line);
            continue;
        }
        struct fixed_random r = {{0}, strcmp(hex, "-") != 0, 0};
        if (r.present)
            hex_decode(hex, r.bytes, sizeof r.bytes);
        uid_uuid u;
        int rc = uid_uuidv7_monotonic_next_custom_random(&st, now, fixed_random, &r, &u);
        CHECK(r.present || r.calls == 0, "drew randomness at %llu", now);
        if (strcmp(want, "error") == 0) {
            CHECK(rc != UID_OK, "expected an error at %llu", now);
        } else {
            char enc[UID_UUID_LEN + 1] = {0};
            uid_uuid_encode(&u, enc);
            CHECK(rc == UID_OK && strcmp(enc, want) == 0, "at %llu got %s (rc %d) want %s", now, enc,
                  rc, want);
        }
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few uuid7_monotonic vectors");
}

struct script_clock {
    uint64_t readings[16];
    int n, pos;
};

static uint64_t script_clock(void *ctx) {
    struct script_clock *c = ctx;
    uint64_t v = c->readings[c->pos];
    if (c->pos < c->n - 1)
        c->pos++;
    return v;
}

static void test_snowflake_sequence_vectors(void) {
    FILE *f = open_vectors("snowflake_sequence.txt");
    char line[512], list[256], want[32];
    unsigned long long epoch = 0, clock_ms;
    unsigned machine = 0;
    int count, rows = 0;
    uint64_t st = 0, id, prev = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "gen %u %llu", &machine, &epoch) == 2) {
            st = 0;
            prev = 0;
        } else if (sscanf(line, "fill %d %llu", &count, &clock_ms) == 2) {
            struct script_clock c = {{clock_ms}, 1, 0};
            for (int i = 0; i < count; i++) {
                c.pos = 0;
                int rc = uid_snowflake_next_clock(&st, machine, epoch, script_clock, &c, &id);
                CHECK(rc == UID_OK && id > prev, "fill %d at %llu: rc %d id %llu", i, clock_ms, rc,
                      (unsigned long long)id);
                prev = id;
            }
        } else if (sscanf(line, "next %255s %31s", list, want) == 2) {
            struct script_clock c = {{0}, 0, 0};
            for (char *p = list; *p && c.n < 16; p++) {
                c.readings[c.n++] = strtoull(p, &p, 10);
                if (*p != ',')
                    break;
            }
            int rc = uid_snowflake_next_clock(&st, machine, epoch, script_clock, &c, &id);
            if (strcmp(want, "error") == 0) {
                CHECK(rc != UID_OK, "expected an error at %s", list);
            } else {
                CHECK(rc == UID_OK && id == strtoull(want, NULL, 10), "at %s got %llu (rc %d) want %s",
                      list, (unsigned long long)id, rc, want);
                prev = id;
            }
        } else {
            CHECK(0, "bad snowflake_sequence line: %s", line);
            continue;
        }
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few snowflake_sequence vectors");
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

/* Decodes hex into a malloc'd buffer; "-" is empty. */
static uint8_t *hex_alloc(const char *hex, size_t *n) {
    size_t len = strcmp(hex, "-") == 0 ? 0 : strlen(hex) / 2;
    uint8_t *out = malloc(len + 1);
    hex_decode(hex, out, len);
    *n = len;
    return out;
}

static void test_hmac_sha256_vectors(void) {
    FILE *f = open_vectors("hmac_sha256.txt");
    static char line[8192], key_hex[1024], msg_hex[4096], want[65];
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%1023s %4095s %64s", key_hex, msg_hex, want) != 3) {
            CHECK(0, "bad hmac line: %.60s", line);
            continue;
        }
        size_t klen, mlen;
        uint8_t *key = hex_alloc(key_hex, &klen), *msg = hex_alloc(msg_hex, &mlen), mac[32];
        char got[65];
        uid_hmac_sha256(key, klen, msg, mlen, mac);
        for (int i = 0; i < 32; i++)
            snprintf(got + 2 * i, 3, "%02x", mac[i]);
        CHECK(strcmp(got, want) == 0, "hmac key %zu msg %zu bytes: got %s want %s", klen, mlen, got,
              want);
        free(key);
        free(msg);
        rows++;
    }
    fclose(f);
    CHECK(rows > 20, "too few hmac vectors");
}

static void test_relid_tag_vectors(void) {
    FILE *f = open_vectors("relid_tag.txt");
    char line[1024], secret_hex[256], salt_hex[256], key_hex[512], want_text[8];
    unsigned long want;
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%255s %255s %511s %lu %7s", secret_hex, salt_hex, key_hex, &want,
                   want_text) != 5) {
            CHECK(0, "bad relid_tag line: %s", line);
            continue;
        }
        size_t slen, saltlen, klen;
        uint8_t *secret = hex_alloc(secret_hex, &slen), *salt = hex_alloc(salt_hex, &saltlen),
                *key = hex_alloc(key_hex, &klen);
        uid_relid_ctx ctx;
        char text[UID_RELID_TAG_LEN + 1] = {0};
        CHECK(uid_relid_ctx_init(&ctx, secret, slen, (char *)salt, saltlen) == UID_OK, "init");
        uint32_t tag = uid_relid_tag(&ctx, (char *)key, klen);
        uid_relid_tag_encode(tag, text);
        CHECK(tag == want && strcmp(text, want_text) == 0, "tag got %u %s want %lu %s", tag, text,
              want, want_text);
        uid_relid_ctx_wipe(&ctx);
        free(secret);
        free(salt);
        free(key);
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few relid_tag vectors");

    uid_relid_ctx ctx;
    CHECK(uid_relid_ctx_init(&ctx, (const uint8_t *)"0123456789abcde", 15, "", 0) == UID_ERR_INVALID,
          "short secret accepted");
}

static void test_relid_vectors(void) {
    FILE *f = open_vectors("relid.txt");
    char line[256], text[64], want[64];
    unsigned long long tag, ms, rnd;
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        uid_relid id, p;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "parts %llu %llu %llu %63s", &tag, &ms, &rnd, want) == 4) {
            int rc = uid_relid_from_parts(&id, (uint32_t)tag, ms, rnd);
            if (tag > UINT32_MAX || strcmp(want, "error") == 0) {
                CHECK(rc == UID_ERR_RANGE, "expected a range error: %s", line);
            } else {
                char enc[UID_RELID_LEN + 1] = {0};
                uid_relid_encode(&id, enc);
                CHECK(rc == UID_OK && strcmp(enc, want) == 0, "got %s want %s", enc, want);
                CHECK(uid_relid_decode(want, strlen(want), &p) == UID_OK && p.tag == tag &&
                          p.timestamp_ms == ms && p.random == rnd,
                      "decode %s", want);
            }
        } else if (sscanf(line, "parse %63s %llu %llu %llu", text, &tag, &ms, &rnd) == 4) {
            CHECK(uid_relid_decode(text, strlen(text), &p) == UID_OK && p.tag == tag &&
                      p.timestamp_ms == ms && p.random == rnd,
                  "parse %s", text);
        } else {
            CHECK(0, "bad relid line: %s", line);
            continue;
        }
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few relid vectors");
}

static void test_relid_invalid(void) {
    FILE *f = open_vectors("relid_invalid.txt");
    char line[256];
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        uid_relid id;
        if (line[0] == '#')
            continue;
        CHECK(len >= 2 && line[0] == '"' && line[len - 1] == '"', "unquoted line: %s", line);
        CHECK(uid_relid_decode(line + 1, len - 2, &id) != UID_OK, "accepted %s", line);
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few relid_invalid vectors");
}

struct fixed_random7 {
    uint8_t bytes[7];
    int present, calls;
};

static int fixed_random7(void *ctx, uint8_t *buf, size_t n) {
    struct fixed_random7 *r = ctx;
    r->calls++;
    if (!r->present || n != sizeof r->bytes)
        return UID_ERR_RANDOM;
    memcpy(buf, r->bytes, n);
    return UID_OK;
}

static void test_relid_monotonic_vectors(void) {
    FILE *f = open_vectors("relid_monotonic.txt");
    char line[256], hex[32], want[32];
    unsigned long long now;
    unsigned long tag;
    uid_relid_monotonic st = {0};
    int rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (strncmp(line, "reset", 5) == 0) {
            memset(&st, 0, sizeof st);
            continue;
        }
        if (sscanf(line, "next %lu %llu %31s %31s", &tag, &now, hex, want) != 4) {
            CHECK(0, "bad relid_monotonic line: %s", line);
            continue;
        }
        struct fixed_random7 r = {{0}, strcmp(hex, "-") != 0, 0};
        if (r.present)
            hex_decode(hex, r.bytes, sizeof r.bytes);
        uid_relid id;
        int rc = uid_relid_monotonic_next_custom_random(&st, (uint32_t)tag, now, fixed_random7, &r,
                                                        &id);
        CHECK(r.present || r.calls == 0, "drew randomness at %llu", now);
        if (strcmp(want, "error") == 0) {
            CHECK(rc != UID_OK, "expected an error at %llu", now);
        } else {
            char enc[UID_RELID_LEN + 1] = {0};
            uid_relid_encode(&id, enc);
            CHECK(rc == UID_OK && strcmp(enc, want) == 0, "at %llu got %s (rc %d) want %s", now,
                  enc, rc, want);
        }
        rows++;
    }
    fclose(f);
    CHECK(rows > 10, "too few relid_monotonic vectors");
}

static void test_relid_generate(void) {
    static const char secret[] = "test-only-secret-0123456789";
    uid_relid_ctx ctx;
    uid_relid a, b;
    uid_relid_monotonic m = {0};
    uint64_t now = uid_now_ms();
    CHECK(uid_relid_ctx_init(&ctx, (const uint8_t *)secret, sizeof secret - 1, "orders", 6) == UID_OK,
          "init");
    uint32_t tag = uid_relid_tag(&ctx, "customer-42", 11);
    CHECK(uid_relid_new(&ctx, "customer-42", 11, &a) == UID_OK &&
              uid_relid_new(&ctx, "customer-42", 11, &b) == UID_OK,
          "new");
    CHECK(a.tag == tag && b.tag == tag && a.random != b.random, "new tag/random");
    CHECK(a.timestamp_ms >= now && a.timestamp_ms < now + 5000, "new timestamp");
    CHECK(uid_relid_monotonic_next(&m, &ctx, "customer-42", 11, &a) == UID_OK, "monotonic first");
    for (int i = 0; i < 1000; i++) {
        const char *key = i % 2 ? "customer-42" : "customer-7";
        CHECK(uid_relid_monotonic_next(&m, &ctx, key, strlen(key), &b) == UID_OK &&
                  (b.timestamp_ms > a.timestamp_ms ||
                   (b.timestamp_ms == a.timestamp_ms && b.random > a.random)),
              "monotonic order");
        a = b;
    }
    uid_relid_ctx_wipe(&ctx);
}

static uint32_t reference_tag(const char *secret, const char *salt, const char *key, size_t key_len) {
    uint8_t msg[128], mac[32];
    size_t salt_len = strlen(salt);
    msg[0] = msg[1] = msg[2] = 0;
    msg[3] = (uint8_t)salt_len;
    memcpy(msg + 4, salt, salt_len);
    memcpy(msg + 4 + salt_len, key, key_len);
    uid_hmac_sha256((const uint8_t *)secret, strlen(secret), msg, 4 + salt_len + key_len, mac);
    return ((uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16 | (uint32_t)mac[2] << 8 | mac[3]) >> 2;
}

static void test_relid_tag_cache(void) {
    static const char secret[] = "test-only-secret-0123456789";
    static const char *salts[] = {"orders", "invoices"};
    char key[UID_RELID_CACHE_KEY + 2];
    uid_relid_ctx ctx;
    for (int s = 0; s < 2; s++) {
        CHECK(uid_relid_ctx_init(&ctx, (const uint8_t *)secret, sizeof secret - 1, salts[s],
                                 strlen(salts[s])) == UID_OK,
              "init");
        for (int round = 0; round < 2; round++) {
            for (int i = 0; i < 4 * UID_RELID_CACHE_SLOTS; i++) {
                size_t len = (size_t)snprintf(key, sizeof key, "customer-%d", i);
                CHECK(uid_relid_tag(&ctx, key, len) == reference_tag(secret, salts[s], key, len),
                      "cached tag");
            }
            for (size_t len = 0; len <= UID_RELID_CACHE_KEY + 1; len++) {
                memset(key, 'k', len);
                CHECK(uid_relid_tag(&ctx, key, len) == reference_tag(secret, salts[s], key, len),
                      "cached tag by length");
            }
        }
    }
    uid_relid_ctx_wipe(&ctx);
}

int main(int argc, char **argv) {
    if (argc > 1)
        testdata = argv[1];
    test_ulid_vectors();
    test_ulid_invalid();
    test_ulid_monotonic();
    test_ulid_monotonic_vectors();
    test_uuid_vectors();
    test_uuid_invalid();
    test_uuid_generate();
    test_uuidv7_monotonic_vectors();
    test_hmac_sha256_vectors();
    test_relid_tag_vectors();
    test_relid_vectors();
    test_relid_invalid();
    test_relid_monotonic_vectors();
    test_relid_generate();
    test_relid_tag_cache();
    test_snowflake_vectors();
    test_snowflake_sequence_vectors();
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
