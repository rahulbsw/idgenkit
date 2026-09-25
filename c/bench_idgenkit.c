#include "idgenkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile uint64_t sink;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

#define BENCH(name, n, body)                                                                       \
    do {                                                                                           \
        double best = 1e300;                                                                       \
        for (int round = 0; round < 5; round++) {                                                  \
            double t0 = now_ns();                                                                  \
            for (long i = 0; i < (n); i++) {                                                       \
                body;                                                                              \
            }                                                                                      \
            double per = (now_ns() - t0) / (n);                                                    \
            if (per < best)                                                                        \
                best = per;                                                                        \
        }                                                                                          \
        printf("c       %-28s %10.1f ns/op %14.0f ops/s\n", name, best, 1e9 / best);               \
    } while (0)

int main(void) {
    long n = getenv("BENCH_N") ? atol(getenv("BENCH_N")) : 1000000;
    printf("# C core, N=%ld\n", n);

    uid_ulid u;
    uid_ulid_monotonic mono = {0};
    uid_uuid uu;
    uid_uuidv7_monotonic uuid_mono = {0};
    uint64_t sf_state = 0, id;
    char text[64];
    uid_ulid_new(&u);
    uid_ulid_encode(&u, text);

    BENCH("ulid.new", n, { uid_ulid_new(&u); sink += u.b[15]; });
    BENCH("ulid.new+encode", n, { uid_ulid_new(&u); uid_ulid_encode(&u, text); sink += text[25]; });
    BENCH("ulid.monotonic", n, { uid_ulid_monotonic_next(&mono, &u); sink += u.b[15]; });
    BENCH("ulid.decode", n, { uid_ulid_decode(text, UID_ULID_LEN, &u); sink += u.b[15]; });
    BENCH("uuid.v4", n, { uid_uuidv4(&uu); sink += uu.b[15]; });
    BENCH("uuid.v7", n, { uid_uuidv7(&uu); sink += uu.b[15]; });
    BENCH("uuid.v7.monotonic", n, { uid_uuidv7_monotonic_next(&uuid_mono, &uu); sink += uu.b[15]; });
    BENCH("snowflake.next", n, { uid_snowflake_next(&sf_state, 1, 0, &id); sink += id; });
    BENCH("nanoid(21)", n, { uid_nanoid(text, 21, uid_nanoid_url_alphabet, 64); sink += text[0]; });
    BENCH("nanoid.custom(hex,21)", n, { uid_nanoid(text, 21, "0123456789abcdef", 16); sink += text[0]; });
    return 0;
}
