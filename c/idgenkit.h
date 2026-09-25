/*
 * idgenkit: dependency-free ULID, Snowflake and Nano ID core in C99.
 *
 * Shared by the PostgreSQL, MySQL and Redis extensions. Requires a POSIX
 * system (Linux, macOS, *BSD) and a GCC/Clang compatible compiler (for the
 * __atomic builtins used by the Snowflake generator).
 */
#ifndef IDGENKIT_H
#define IDGENKIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UID_OK = 0,
    UID_ERR_RANDOM = -1,   /* OS random source failed */
    UID_ERR_RANGE = -2,    /* timestamp/argument outside the representable range */
    UID_ERR_OVERFLOW = -3, /* monotonic ULID random component exhausted */
    UID_ERR_INVALID = -4   /* malformed input */
};

const char *uid_strerror(int err);

/* Fill buf from the OS CSPRNG. Fork-safe: no userspace pooling. */
int uid_random_bytes(uint8_t *buf, size_t len);
uint64_t uid_now_ms(void);

typedef int (*uid_random_fn)(void *ctx, uint8_t *buf, size_t len);
typedef uint64_t (*uid_clock_fn)(void *ctx);

/* ---- ULID (https://github.com/ulid/spec) -------------------------------- */

#define UID_ULID_LEN 26
#define UID_ULID_MAX_TIME ((UINT64_C(1) << 48) - 1)

typedef struct {
    uint8_t b[16]; /* big-endian: 6 bytes time, 10 bytes randomness */
} uid_ulid;

/* Caller-owned monotonic state; zero-initialise before first use. Not
 * thread-safe: use one per thread/process, or guard it externally. */
typedef struct {
    uid_ulid last;
    int primed;
} uid_ulid_monotonic;

int uid_ulid_new(uid_ulid *out);
int uid_ulid_from_parts(uid_ulid *out, uint64_t timestamp_ms, const uint8_t random[10]);
int uid_ulid_monotonic_next(uid_ulid_monotonic *state, uid_ulid *out);
int uid_ulid_monotonic_next_at(uid_ulid_monotonic *state, uint64_t now_ms, uid_ulid *out);
int uid_ulid_monotonic_next_custom_random(uid_ulid_monotonic *state, uint64_t now_ms,
                                          uid_random_fn random, void *ctx, uid_ulid *out);
uint64_t uid_ulid_timestamp(const uid_ulid *u);
/* Writes exactly UID_ULID_LEN bytes; does not NUL-terminate. */
void uid_ulid_encode(const uid_ulid *u, char out[UID_ULID_LEN]);
/* Case-insensitive; rejects wrong length, invalid characters and >128-bit values. */
int uid_ulid_decode(const char *text, size_t len, uid_ulid *out);

/* ---- Snowflake (42-bit ms | 10-bit machine | 12-bit sequence) ------------ */

#define UID_SNOWFLAKE_MAX_MACHINE_ID 1023u
#define UID_SNOWFLAKE_MAX_SEQUENCE 4095u
#define UID_SNOWFLAKE_MAX_DELTA ((UINT64_C(1) << 42) - 1)

/*
 * Lock-free generator. *state is an opaque word, initialised to 0, that may
 * live in shared memory and be used concurrently by threads or processes
 * (it is only accessed with atomic CAS). One state must be shared by every
 * caller using the same machine id.
 */
int uid_snowflake_next(uint64_t *state, uint32_t machine_id, uint64_t epoch_ms, uint64_t *out);
int uid_snowflake_next_clock(uint64_t *state, uint32_t machine_id, uint64_t epoch_ms,
                             uid_clock_fn clock, void *ctx, uint64_t *out);
int uid_snowflake_compose(uint64_t timestamp_ms, uint32_t machine_id, uint32_t sequence,
                          uint64_t epoch_ms, uint64_t *out);
void uid_snowflake_parse(uint64_t id, uint64_t epoch_ms, uint64_t *timestamp_ms,
                         uint32_t *machine_id, uint32_t *sequence);

/* ---- Nano ID (https://github.com/ai/nanoid) ------------------------------ */

#define UID_NANOID_DEFAULT_SIZE 21
extern const char uid_nanoid_url_alphabet[];

/* Writes exactly `size` bytes to out (no NUL). The alphabet must be 1..256
 * ASCII bytes; multi-byte symbols are rejected with UID_ERR_INVALID. */
int uid_nanoid(char *out, size_t size, const char *alphabet, size_t alphabet_len);
int uid_nanoid_custom_random(char *out, size_t size, const char *alphabet, size_t alphabet_len,
                             uid_random_fn random, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
