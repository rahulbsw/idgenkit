/*
 * idgenkit: dependency-free ULID, UUIDv4/v7, relative ID, Snowflake and Nano ID
 * core in C99.
 *
 * Shared by the PostgreSQL, MySQL and Redis extensions. Requires a POSIX
 * system (Linux, macOS, *BSD) and a GCC/Clang compatible compiler (for the
 * __atomic builtins used by the Snowflake generator).
 *
 * There is no shared library: each extension compiles idgenkit.c into its own
 * module, so a change here needs every extension rebuilt. MySQL and Redis pass
 * ../c/idgenkit.c to the compiler. PostgreSQL builds it through
 * postgres/src/idgenkit_core.c, which #includes "../../c/idgenkit.c" because
 * PGXS only compiles sources listed in OBJS; c/ must stay beside postgres/
 * (postgres/Dockerfile copies both to /src).
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
    UID_ERR_OVERFLOW = -3, /* monotonic random component exhausted */
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

/* ---- UUIDv4 / UUIDv7 (RFC 9562) ------------------------------------------ */

#define UID_UUID_LEN 36
#define UID_UUIDV7_MAX_TIME ((UINT64_C(1) << 48) - 1)

typedef struct {
    uint8_t b[16]; /* big-endian, as in the text form */
} uid_uuid;

/* Caller-owned monotonic state; zero-initialise before first use. Not
 * thread-safe: use one per thread/process, or guard it externally. */
typedef struct {
    uid_uuid last;
    int primed;
} uid_uuidv7_monotonic;

int uid_uuidv4(uid_uuid *out);
/* Sets the version and variant bits of `random`; always succeeds. */
void uid_uuidv4_from_random(uid_uuid *out, const uint8_t random[16]);
int uid_uuidv7(uid_uuid *out);
int uid_uuidv7_from_parts(uid_uuid *out, uint64_t timestamp_ms, const uint8_t random[10]);
int uid_uuidv7_monotonic_next(uid_uuidv7_monotonic *state, uid_uuid *out);
int uid_uuidv7_monotonic_next_at(uid_uuidv7_monotonic *state, uint64_t now_ms, uid_uuid *out);
int uid_uuidv7_monotonic_next_custom_random(uid_uuidv7_monotonic *state, uint64_t now_ms,
                                            uid_random_fn random, void *ctx, uid_uuid *out);
unsigned uid_uuid_version(const uid_uuid *u);
/* The 48-bit timestamp of a version 7 UUID; UID_ERR_INVALID for other versions. */
int uid_uuidv7_timestamp(const uid_uuid *u, uint64_t *timestamp_ms);
/* Writes exactly UID_UUID_LEN lowercase bytes; does not NUL-terminate. */
void uid_uuid_encode(const uid_uuid *u, char out[UID_UUID_LEN]);
/* Case-insensitive; accepts only the 36-character hyphenated form. */
int uid_uuid_decode(const char *text, size_t len, uid_uuid *out);

/* ---- Relative ID (30-bit keyed tag | 48-bit ms | 50-bit random) ---------- */

#define UID_RELID_LEN 28
#define UID_RELID_TAG_LEN 6
#define UID_RELID_MIN_SECRET 16
#define UID_RELID_MAX_TAG ((UINT32_C(1) << 30) - 1)
#define UID_RELID_MAX_TIME ((UINT64_C(1) << 48) - 1)
#define UID_RELID_MAX_RANDOM ((UINT64_C(1) << 50) - 1)

typedef struct {
    uint32_t tag;
    uint64_t timestamp_ms;
    uint64_t random;
} uid_relid;

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} uid_sha256;

#define UID_RELID_CACHE_SLOTS 64
#define UID_RELID_CACHE_KEY 32

typedef struct {
    uint32_t tag;
    uint8_t len; /* key length + 1; 0 for an empty slot */
    char key[UID_RELID_CACHE_KEY];
} uid_relid_cache_entry;

/* HMAC-SHA-256 keyed with the secret, with BE32(len(salt)) || salt already
 * absorbed, plus the tags of recent keys up to UID_RELID_CACHE_KEY bytes.
 * Not thread-safe: use one per thread. Holds secret-equivalent material:
 * wipe it when done. */
typedef struct {
    uid_sha256 inner, outer;
    uid_relid_cache_entry cache[UID_RELID_CACHE_SLOTS];
} uid_relid_ctx;

/* Caller-owned monotonic state shared by every key; zero-initialise before
 * first use. Not thread-safe: use one per thread/process, or guard it. */
typedef struct {
    uint64_t last_ms;
    uint64_t last_rand;
    int primed;
} uid_relid_monotonic;

void uid_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]);

/* UID_ERR_INVALID if the secret is shorter than UID_RELID_MIN_SECRET bytes. */
int uid_relid_ctx_init(uid_relid_ctx *ctx, const uint8_t *secret, size_t secret_len,
                       const char *salt, size_t salt_len);
void uid_relid_ctx_wipe(uid_relid_ctx *ctx);
uint32_t uid_relid_tag(uid_relid_ctx *ctx, const char *key, size_t key_len);
int uid_relid_new(uid_relid_ctx *ctx, const char *key, size_t key_len, uid_relid *out);
int uid_relid_from_parts(uid_relid *out, uint32_t tag, uint64_t timestamp_ms, uint64_t random);
int uid_relid_monotonic_next(uid_relid_monotonic *state, uid_relid_ctx *ctx, const char *key,
                             size_t key_len, uid_relid *out);
int uid_relid_monotonic_next_custom_random(uid_relid_monotonic *state, uint32_t tag,
                                           uint64_t now_ms, uid_random_fn random, void *rctx,
                                           uid_relid *out);
/* Writes exactly UID_RELID_LEN bytes (TTTTTT-MMMMMMMMMM-RRRRRRRRRR); no NUL. */
void uid_relid_encode(const uid_relid *id, char out[UID_RELID_LEN]);
/* Writes exactly UID_RELID_TAG_LEN bytes; no NUL. */
void uid_relid_tag_encode(uint32_t tag, char out[UID_RELID_TAG_LEN]);
/* Case-insensitive; accepts the 28-character form or 26 characters without hyphens. */
int uid_relid_decode(const char *text, size_t len, uid_relid *out);

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
