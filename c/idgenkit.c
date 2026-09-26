#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "idgenkit.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
#include <stdlib.h>
#define UID_HAVE_ARC4RANDOM 1
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#else
#error "idgenkit: unsupported platform (need arc4random_buf or getrandom)"
#endif

const char *uid_strerror(int err) {
    switch (err) {
    case UID_OK:
        return "ok";
    case UID_ERR_RANDOM:
        return "OS random source failed";
    case UID_ERR_RANGE:
        return "value out of range";
    case UID_ERR_OVERFLOW:
        return "monotonic random component overflow";
    case UID_ERR_INVALID:
        return "invalid input";
    default:
        return "unknown error";
    }
}

int uid_random_bytes(uint8_t *buf, size_t len) {
#if defined(__APPLE__)
    /* macOS arc4random_buf is ~6x slower per call above 10 bytes. */
    while (len > 10) {
        arc4random_buf(buf, 10);
        buf += 10;
        len -= 10;
    }
    arc4random_buf(buf, len);
    return UID_OK;
#elif defined(UID_HAVE_ARC4RANDOM)
    arc4random_buf(buf, len);
    return UID_OK;
#else
    while (len > 0) {
        ssize_t n = getrandom(buf, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return UID_ERR_RANDOM;
        }
        buf += n;
        len -= (size_t)n;
    }
    return UID_OK;
#endif
}

static int os_random(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    return uid_random_bytes(buf, len);
}

uint64_t uid_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t os_clock(void *ctx) {
    (void)ctx;
    return uid_now_ms();
}

static void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/* ---- ULID ---------------------------------------------------------------- */

static const char ULID_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Symbol value + 1, so that unlisted (zero) entries mean "invalid". */
#define D(c, v) [c] = (v) + 1, [c + ('a' - 'A')] = (v) + 1
static const uint8_t ULID_DECODE[256] = {
    ['0'] = 1, ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8,
    ['8'] = 9, ['9'] = 10, D('A', 10), D('B', 11), D('C', 12), D('D', 13), D('E', 14),
    D('F', 15), D('G', 16), D('H', 17), D('J', 18), D('K', 19), D('M', 20), D('N', 21),
    D('P', 22), D('Q', 23), D('R', 24), D('S', 25), D('T', 26), D('V', 27), D('W', 28),
    D('X', 29), D('Y', 30), D('Z', 31),
};
#undef D

static int8_t ulid_decode_char(unsigned char c) {
    return (int8_t)(ULID_DECODE[c] - 1);
}

static void put_time(uid_ulid *u, uint64_t ms) {
    for (int i = 5; i >= 0; i--) {
        u->b[i] = (uint8_t)ms;
        ms >>= 8;
    }
}

uint64_t uid_ulid_timestamp(const uid_ulid *u) {
    uint64_t ms = 0;
    for (int i = 0; i < 6; i++)
        ms = ms << 8 | u->b[i];
    return ms;
}

int uid_ulid_from_parts(uid_ulid *out, uint64_t timestamp_ms, const uint8_t random[10]) {
    if (timestamp_ms > UID_ULID_MAX_TIME)
        return UID_ERR_RANGE;
    put_time(out, timestamp_ms);
    memcpy(out->b + 6, random, 10);
    return UID_OK;
}

int uid_ulid_new(uid_ulid *out) {
    uint64_t now = uid_now_ms();
    if (now > UID_ULID_MAX_TIME)
        return UID_ERR_RANGE;
    put_time(out, now);
    return uid_random_bytes(out->b + 6, 10);
}

int uid_ulid_monotonic_next_at(uid_ulid_monotonic *st, uint64_t now_ms, uid_ulid *out) {
    return uid_ulid_monotonic_next_custom_random(st, now_ms, os_random, NULL, out);
}

int uid_ulid_monotonic_next_custom_random(uid_ulid_monotonic *st, uint64_t now_ms,
                                          uid_random_fn random, void *ctx, uid_ulid *out) {
    uid_ulid next;
    int rc;

    if (st->primed && now_ms <= uid_ulid_timestamp(&st->last)) {
        int i = 15;
        while (i >= 6 && st->last.b[i] == 0xFF)
            i--;
        if (i < 6)
            return UID_ERR_OVERFLOW;
        st->last.b[i]++;
        memset(st->last.b + i + 1, 0, (size_t)(15 - i));
        *out = st->last;
        return UID_OK;
    }
    if (now_ms > UID_ULID_MAX_TIME)
        return UID_ERR_RANGE;
    put_time(&next, now_ms);
    rc = random(ctx, next.b + 6, 10);
    if (rc != UID_OK)
        return rc;
    st->last = next;
    st->primed = 1;
    *out = next;
    return UID_OK;
}

int uid_ulid_monotonic_next(uid_ulid_monotonic *st, uid_ulid *out) {
    return uid_ulid_monotonic_next_at(st, uid_now_ms(), out);
}

void uid_ulid_encode(const uid_ulid *u, char out[UID_ULID_LEN]) {
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) {
        hi = hi << 8 | u->b[i];
        lo = lo << 8 | u->b[i + 8];
    }
    for (int i = UID_ULID_LEN - 1; i >= 0; i--) {
        out[i] = ULID_ALPHABET[lo & 31];
        lo = lo >> 5 | hi << 59;
        hi >>= 5;
    }
}

int uid_ulid_decode(const char *text, size_t len, uid_ulid *out) {
    uint64_t hi = 0, lo = 0;

    if (len != UID_ULID_LEN)
        return UID_ERR_INVALID;
    for (size_t i = 0; i < UID_ULID_LEN; i++) {
        int8_t v = ulid_decode_char((unsigned char)text[i]);
        if (v < 0 || (i == 0 && v > 7))
            return UID_ERR_INVALID;
        hi = hi << 5 | lo >> 59;
        lo = lo << 5 | (uint64_t)v;
    }
    for (int i = 7; i >= 0; i--) {
        out->b[i] = (uint8_t)hi;
        out->b[i + 8] = (uint8_t)lo;
        hi >>= 8;
        lo >>= 8;
    }
    return UID_OK;
}

/* ---- UUIDv4 / UUIDv7 ------------------------------------------------------ */

#define UUID_RAND_B_MAX ((UINT64_C(1) << 62) - 1)

static void set_version(uid_uuid *u, unsigned version) {
    u->b[6] = (uint8_t)((u->b[6] & 0x0F) | (version << 4));
    u->b[8] = (uint8_t)((u->b[8] & 0x3F) | 0x80);
}

void uid_uuidv4_from_random(uid_uuid *out, const uint8_t random[16]) {
    memcpy(out->b, random, 16);
    set_version(out, 4);
}

int uid_uuidv4(uid_uuid *out) {
    int rc = uid_random_bytes(out->b, 16);
    if (rc == UID_OK)
        set_version(out, 4);
    return rc;
}

int uid_uuidv7_from_parts(uid_uuid *out, uint64_t timestamp_ms, const uint8_t random[10]) {
    if (timestamp_ms > UID_UUIDV7_MAX_TIME)
        return UID_ERR_RANGE;
    for (int i = 5; i >= 0; i--, timestamp_ms >>= 8)
        out->b[i] = (uint8_t)timestamp_ms;
    memcpy(out->b + 6, random, 10);
    set_version(out, 7);
    return UID_OK;
}

int uid_uuidv7(uid_uuid *out) {
    uint8_t random[10];
    int rc = uid_random_bytes(random, sizeof random);
    return rc == UID_OK ? uid_uuidv7_from_parts(out, uid_now_ms(), random) : rc;
}

unsigned uid_uuid_version(const uid_uuid *u) {
    return u->b[6] >> 4;
}

int uid_uuidv7_timestamp(const uid_uuid *u, uint64_t *timestamp_ms) {
    uint64_t ms = 0;
    if (uid_uuid_version(u) != 7)
        return UID_ERR_INVALID;
    for (int i = 0; i < 6; i++)
        ms = ms << 8 | u->b[i];
    *timestamp_ms = ms;
    return UID_OK;
}

/* Adds one to the 74 random bits (rand_a << 62 | rand_b), skipping the
 * version and variant bits. Returns 0 if they are already all ones. */
static int uuidv7_increment(uid_uuid *u) {
    uint64_t rand_a = (uint64_t)(u->b[6] & 0x0F) << 8 | u->b[7];
    uint64_t rand_b = (uint64_t)(u->b[8] & 0x3F);
    for (int i = 9; i < 16; i++)
        rand_b = rand_b << 8 | u->b[i];
    if (rand_b < UUID_RAND_B_MAX) {
        rand_b++;
    } else if (rand_a < 0xFFF) {
        rand_a++;
        rand_b = 0;
    } else {
        return 0;
    }
    u->b[6] = (uint8_t)(0x70 | rand_a >> 8);
    u->b[7] = (uint8_t)rand_a;
    for (int i = 15; i >= 9; i--, rand_b >>= 8)
        u->b[i] = (uint8_t)rand_b;
    u->b[8] = (uint8_t)(0x80 | rand_b);
    return 1;
}

int uid_uuidv7_monotonic_next_custom_random(uid_uuidv7_monotonic *st, uint64_t now_ms,
                                            uid_random_fn random, void *ctx, uid_uuid *out) {
    uint64_t last_ms;
    uint8_t buf[10];
    uid_uuid next;
    int rc;

    if (st->primed && uid_uuidv7_timestamp(&st->last, &last_ms) == UID_OK && now_ms <= last_ms) {
        next = st->last;
        if (!uuidv7_increment(&next))
            return UID_ERR_OVERFLOW;
        st->last = next;
        *out = next;
        return UID_OK;
    }
    if (now_ms > UID_UUIDV7_MAX_TIME)
        return UID_ERR_RANGE;
    rc = random(ctx, buf, sizeof buf);
    if (rc != UID_OK)
        return rc;
    uid_uuidv7_from_parts(&next, now_ms, buf);
    st->last = next;
    st->primed = 1;
    *out = next;
    return UID_OK;
}

int uid_uuidv7_monotonic_next_at(uid_uuidv7_monotonic *st, uint64_t now_ms, uid_uuid *out) {
    return uid_uuidv7_monotonic_next_custom_random(st, now_ms, os_random, NULL, out);
}

int uid_uuidv7_monotonic_next(uid_uuidv7_monotonic *st, uid_uuid *out) {
    return uid_uuidv7_monotonic_next_at(st, uid_now_ms(), out);
}

static const char HEX[16] = "0123456789abcdef";

void uid_uuid_encode(const uid_uuid *u, char out[UID_UUID_LEN]) {
    for (int i = 0, o = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = HEX[u->b[i] >> 4];
        out[o++] = HEX[u->b[i] & 15];
    }
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int uid_uuid_decode(const char *text, size_t len, uid_uuid *out) {
    uid_uuid u;
    if (len != UID_UUID_LEN)
        return UID_ERR_INVALID;
    for (int i = 0, t = 0; i < 16; i++) {
        if (t == 8 || t == 13 || t == 18 || t == 23) {
            if (text[t] != '-')
                return UID_ERR_INVALID;
            t++;
        }
        int hi = hex_value((unsigned char)text[t]), lo = hex_value((unsigned char)text[t + 1]);
        if (hi < 0 || lo < 0)
            return UID_ERR_INVALID;
        u.b[i] = (uint8_t)(hi << 4 | lo);
        t += 2;
    }
    *out = u;
    return UID_OK;
}

/* ---- SHA-256 (FIPS 180-4) and HMAC (RFC 2104) ----------------------------- */

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR32(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha256_block(uint32_t h[8], const uint8_t p[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25)) + ((e & f) ^ (~e & g)) +
                      SHA256_K[i] + w[i];
        uint32_t t2 = (ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void sha256_init(uid_sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->used = 0;
}

static void sha256_update(uid_sha256 *s, const uint8_t *p, size_t n) {
    s->total += n;
    if (s->used > 0) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used < 64)
            return;
        sha256_block(s->h, s->buf);
        s->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64)
        sha256_block(s->h, p);
    memcpy(s->buf, p, n);
    s->used = n;
}

static void sha256_final(uid_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->total * 8;
    uint8_t pad[72] = {0x80};
    size_t padlen = (s->used < 56 ? 56 : 120) - s->used;
    for (int i = 0; i < 8; i++)
        pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

static void hmac_sha256_init(uid_sha256 *inner, uid_sha256 *outer, const uint8_t *key,
                             size_t key_len) {
    uint8_t block[64] = {0};
    if (key_len > 64) {
        uid_sha256 s;
        sha256_init(&s);
        sha256_update(&s, key, key_len);
        sha256_final(&s, block);
        wipe(&s, sizeof s);
    } else if (key_len > 0) {
        memcpy(block, key, key_len);
    }
    for (int i = 0; i < 64; i++)
        block[i] ^= 0x36;
    sha256_init(inner);
    sha256_update(inner, block, 64);
    for (int i = 0; i < 64; i++)
        block[i] ^= 0x36 ^ 0x5c;
    sha256_init(outer);
    sha256_update(outer, block, 64);
    wipe(block, sizeof block);
}

/* Consumes copies of the prepared states; the caller's stay reusable. */
static void hmac_sha256_finish(uid_sha256 inner, uid_sha256 outer, const uint8_t *msg,
                               size_t msg_len, uint8_t out[32]) {
    uint8_t digest[32];
    sha256_update(&inner, msg, msg_len);
    sha256_final(&inner, digest);
    sha256_update(&outer, digest, sizeof digest);
    sha256_final(&outer, out);
}

void uid_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]) {
    uid_sha256 inner, outer;
    hmac_sha256_init(&inner, &outer, key, key_len);
    hmac_sha256_finish(inner, outer, msg, msg_len, out);
    wipe(&inner, sizeof inner);
    wipe(&outer, sizeof outer);
}

/* ---- Relative ID ----------------------------------------------------------- */

int uid_relid_ctx_init(uid_relid_ctx *ctx, const uint8_t *secret, size_t secret_len,
                       const char *salt, size_t salt_len) {
    uint8_t prefix[4];
    if (secret_len < UID_RELID_MIN_SECRET || (uint64_t)salt_len > UINT32_MAX)
        return UID_ERR_INVALID;
    hmac_sha256_init(&ctx->inner, &ctx->outer, secret, secret_len);
    for (int i = 0; i < 4; i++)
        prefix[i] = (uint8_t)((uint64_t)salt_len >> (24 - 8 * i));
    sha256_update(&ctx->inner, prefix, 4);
    sha256_update(&ctx->inner, (const uint8_t *)salt, salt_len);
    return UID_OK;
}

void uid_relid_ctx_wipe(uid_relid_ctx *ctx) {
    wipe(ctx, sizeof *ctx);
}

uint32_t uid_relid_tag(const uid_relid_ctx *ctx, const char *key, size_t key_len) {
    uint8_t mac[32];
    hmac_sha256_finish(ctx->inner, ctx->outer, (const uint8_t *)key, key_len, mac);
    return ((uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16 | (uint32_t)mac[2] << 8 | mac[3]) >> 2;
}

int uid_relid_from_parts(uid_relid *out, uint32_t tag, uint64_t timestamp_ms, uint64_t random) {
    if (tag > UID_RELID_MAX_TAG || timestamp_ms > UID_RELID_MAX_TIME ||
        random > UID_RELID_MAX_RANDOM)
        return UID_ERR_RANGE;
    out->tag = tag;
    out->timestamp_ms = timestamp_ms;
    out->random = random;
    return UID_OK;
}

static int relid_random(uid_random_fn random, void *rctx, uint64_t *out) {
    uint8_t buf[7];
    uint64_t v = 0;
    int rc = random(rctx, buf, sizeof buf);
    if (rc != UID_OK)
        return rc;
    for (int i = 0; i < 7; i++)
        v = v << 8 | buf[i];
    *out = v & UID_RELID_MAX_RANDOM;
    return UID_OK;
}

int uid_relid_new(const uid_relid_ctx *ctx, const char *key, size_t key_len, uid_relid *out) {
    uint64_t now = uid_now_ms(), rand;
    int rc;
    if (now > UID_RELID_MAX_TIME)
        return UID_ERR_RANGE;
    rc = relid_random(os_random, NULL, &rand);
    if (rc != UID_OK)
        return rc;
    return uid_relid_from_parts(out, uid_relid_tag(ctx, key, key_len), now, rand);
}

int uid_relid_monotonic_next_custom_random(uid_relid_monotonic *st, uint32_t tag,
                                           uint64_t now_ms, uid_random_fn random, void *rctx,
                                           uid_relid *out) {
    uint64_t rand;
    int rc;
    if (tag > UID_RELID_MAX_TAG)
        return UID_ERR_RANGE;
    if (st->primed && now_ms <= st->last_ms) {
        if (st->last_rand == UID_RELID_MAX_RANDOM)
            return UID_ERR_OVERFLOW;
        st->last_rand++;
    } else {
        if (now_ms > UID_RELID_MAX_TIME)
            return UID_ERR_RANGE;
        rc = relid_random(random, rctx, &rand);
        if (rc != UID_OK)
            return rc;
        st->last_ms = now_ms;
        st->last_rand = rand;
        st->primed = 1;
    }
    return uid_relid_from_parts(out, tag, st->last_ms, st->last_rand);
}

int uid_relid_monotonic_next(uid_relid_monotonic *st, const uid_relid_ctx *ctx, const char *key,
                             size_t key_len, uid_relid *out) {
    return uid_relid_monotonic_next_custom_random(st, uid_relid_tag(ctx, key, key_len),
                                                  uid_now_ms(), os_random, NULL, out);
}

static void relid_put(char *out, uint64_t v, int width) {
    for (int i = width - 1; i >= 0; i--, v >>= 5)
        out[i] = ULID_ALPHABET[v & 31];
}

void uid_relid_tag_encode(uint32_t tag, char out[UID_RELID_TAG_LEN]) {
    relid_put(out, tag, 6);
}

void uid_relid_encode(const uid_relid *id, char out[UID_RELID_LEN]) {
    relid_put(out, id->tag, 6);
    out[6] = '-';
    relid_put(out + 7, id->timestamp_ms, 10);
    out[17] = '-';
    relid_put(out + 18, id->random, 10);
}

static int relid_get(const char *text, int width, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < width; i++) {
        int8_t d = ulid_decode_char((unsigned char)text[i]);
        if (d < 0)
            return UID_ERR_INVALID;
        v = v << 5 | (uint64_t)d;
    }
    *out = v;
    return UID_OK;
}

int uid_relid_decode(const char *text, size_t len, uid_relid *out) {
    uint64_t tag, ms, rand;
    int sep;
    if (len == UID_RELID_LEN && text[6] == '-' && text[17] == '-')
        sep = 1;
    else if (len == UID_RELID_LEN - 2)
        sep = 0;
    else
        return UID_ERR_INVALID;
    if (relid_get(text, 6, &tag) != UID_OK || relid_get(text + 6 + sep, 10, &ms) != UID_OK ||
        relid_get(text + 16 + 2 * sep, 10, &rand) != UID_OK || ms > UID_RELID_MAX_TIME)
        return UID_ERR_INVALID;
    return uid_relid_from_parts(out, (uint32_t)tag, ms, rand);
}

/* ---- Snowflake ------------------------------------------------------------ */

#define SF_SEQ_BITS 12
#define SF_MACHINE_SHIFT 12
#define SF_TIME_SHIFT 22

int uid_snowflake_compose(uint64_t timestamp_ms, uint32_t machine_id, uint32_t sequence,
                          uint64_t epoch_ms, uint64_t *out) {
    if (machine_id > UID_SNOWFLAKE_MAX_MACHINE_ID || sequence > UID_SNOWFLAKE_MAX_SEQUENCE)
        return UID_ERR_RANGE;
    if (timestamp_ms < epoch_ms || timestamp_ms - epoch_ms > UID_SNOWFLAKE_MAX_DELTA)
        return UID_ERR_RANGE;
    *out = (timestamp_ms - epoch_ms) << SF_TIME_SHIFT | (uint64_t)machine_id << SF_MACHINE_SHIFT |
           sequence;
    return UID_OK;
}

void uid_snowflake_parse(uint64_t id, uint64_t epoch_ms, uint64_t *timestamp_ms,
                         uint32_t *machine_id, uint32_t *sequence) {
    *timestamp_ms = (id >> SF_TIME_SHIFT) + epoch_ms;
    *machine_id = (uint32_t)(id >> SF_MACHINE_SHIFT) & UID_SNOWFLAKE_MAX_MACHINE_ID;
    *sequence = (uint32_t)id & UID_SNOWFLAKE_MAX_SEQUENCE;
}

int uid_snowflake_next(uint64_t *state, uint32_t machine_id, uint64_t epoch_ms, uint64_t *out) {
    return uid_snowflake_next_clock(state, machine_id, epoch_ms, os_clock, NULL, out);
}

int uid_snowflake_next_clock(uint64_t *state, uint32_t machine_id, uint64_t epoch_ms,
                             uid_clock_fn clock, void *ctx, uint64_t *out) {
    if (machine_id > UID_SNOWFLAKE_MAX_MACHINE_ID)
        return UID_ERR_RANGE;
    for (;;) {
        uint64_t old = __atomic_load_n(state, __ATOMIC_ACQUIRE);
        uint64_t last_ms = old >> SF_SEQ_BITS, seq = old & UID_SNOWFLAKE_MAX_SEQUENCE;
        uint64_t now = clock(ctx), ms, next, desired;

        if (now > last_ms) {
            ms = now;
            next = 0;
        } else if (seq < UID_SNOWFLAKE_MAX_SEQUENCE) {
            /* same millisecond, or the clock moved backwards: keep last_ms */
            ms = last_ms;
            next = seq + 1;
        } else {
            while (clock(ctx) <= last_ms)
                cpu_relax();
            continue;
        }
        if (ms < epoch_ms || ms - epoch_ms > UID_SNOWFLAKE_MAX_DELTA)
            return UID_ERR_RANGE;
        desired = ms << SF_SEQ_BITS | next;
        if (__atomic_compare_exchange_n(state, &old, desired, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            *out = (ms - epoch_ms) << SF_TIME_SHIFT | (uint64_t)machine_id << SF_MACHINE_SHIFT | next;
            return UID_OK;
        }
    }
}

/* ---- Nano ID -------------------------------------------------------------- */

const char uid_nanoid_url_alphabet[] =
    "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict";

int uid_nanoid_custom_random(char *out, size_t size, const char *alphabet, size_t alen,
                             uid_random_fn random, void *ctx) {
    uint8_t buf[256];
    size_t count = 0, mask, step;
    unsigned bits;

    if (size == 0 || alen == 0 || alen > 256)
        return UID_ERR_INVALID;
    for (size_t i = 0; i < alen; i++) {
        if ((unsigned char)alphabet[i] >= 0x80)
            return UID_ERR_INVALID;
    }
    bits = 32u - (unsigned)__builtin_clz((unsigned)(alen - 1) | 1u);
    mask = (2u << (bits - 1)) - 1;

    if (mask + 1 == alen) {
        while (count < size) {
            size_t n = size - count < sizeof buf ? size - count : sizeof buf;
            int rc = random(ctx, buf, n);
            if (rc != UID_OK)
                return rc;
            for (size_t i = 0; i < n; i++)
                out[count++] = alphabet[buf[i] & mask];
        }
        return UID_OK;
    }
    /* Exact integer form of nanoid's ceil(1.6 * mask * size / alphabet_len). */
    step = (8 * mask * size + 5 * alen - 1) / (5 * alen);
    for (;;) {
        /* Large sizes are served in chunks; each chunk is one random() call. */
        size_t n = step < sizeof buf ? step : sizeof buf;
        int rc = random(ctx, buf, n);
        if (rc != UID_OK)
            return rc;
        for (size_t i = 0; i < n; i++) {
            size_t idx = buf[i] & mask;
            if (idx < alen) {
                out[count++] = alphabet[idx];
                if (count == size)
                    return UID_OK;
            }
        }
    }
}

int uid_nanoid(char *out, size_t size, const char *alphabet, size_t alen) {
    return uid_nanoid_custom_random(out, size, alphabet, alen, os_random, NULL);
}
