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
        return "monotonic ULID random component overflow";
    case UID_ERR_INVALID:
        return "invalid input";
    default:
        return "unknown error";
    }
}

int uid_random_bytes(uint8_t *buf, size_t len) {
#ifdef UID_HAVE_ARC4RANDOM
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

uint64_t uid_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
    rc = uid_random_bytes(next.b + 6, 10);
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
    if (machine_id > UID_SNOWFLAKE_MAX_MACHINE_ID)
        return UID_ERR_RANGE;
    for (;;) {
        uint64_t old = __atomic_load_n(state, __ATOMIC_ACQUIRE);
        uint64_t last_ms = old >> SF_SEQ_BITS, seq = old & UID_SNOWFLAKE_MAX_SEQUENCE;
        uint64_t now = uid_now_ms(), ms, next, desired;

        if (now > last_ms) {
            ms = now;
            next = 0;
        } else if (seq < UID_SNOWFLAKE_MAX_SEQUENCE) {
            /* same millisecond, or the clock moved backwards: keep last_ms */
            ms = last_ms;
            next = seq + 1;
        } else {
            while (uid_now_ms() <= last_ms)
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

static int os_random(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    return uid_random_bytes(buf, len);
}

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
