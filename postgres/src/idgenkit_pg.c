/*
 * PostgreSQL extension: ULID, UUIDv7, relative ID, Snowflake and Nano ID generation.
 *
 * Snowflake state must be shared by every backend process, so it lives in
 * shared memory: static shared memory when loaded via shared_preload_libraries,
 * otherwise (PostgreSQL 17+) a named DSM segment. The machine id and epoch are
 * captured into that shared struct when it is created, so every backend uses
 * one configuration until the server restarts.
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#if PG_VERSION_NUM >= 170000
#include "storage/dsm_registry.h"
#endif

#include "idgenkit.h"

PG_MODULE_MAGIC;

#define NANOID_MAX_SIZE 1024
#define UNIX_EPOCH_TO_PG_EPOCH_MS \
    ((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * 1000)

void _PG_init(void);

static int machine_id = 1;
static char *snowflake_epoch_guc = NULL;
static uint64 snowflake_epoch_ms = 0;
static char *relid_secret = NULL;
static bool preloaded = false;

typedef struct SnowflakeShared {
    uint64 state;
    uint64 epoch_ms;
    uint32 machine_id;
} SnowflakeShared;

static SnowflakeShared *snowflake_shared = NULL;
static uid_ulid_monotonic ulid_mono;
static uid_uuidv7_monotonic uuidv7_mono;
static uid_relid_monotonic relid_mono;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif

/* ---- configuration and shared memory ------------------------------------- */

static bool parse_epoch(const char *s, uint64 *out) {
    char *end;
    unsigned long long v;

    if (s == NULL || *s == '\0' || *s == '-')
        return false;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;
    *out = (uint64) v;
    return true;
}

static bool check_epoch(char **newval, void **extra, GucSource source) {
    uint64 v;

    if (!parse_epoch(*newval, &v)) {
        GUC_check_errdetail("idgenkit.snowflake_epoch_ms must be a non-negative integer (ms since 1970-01-01).");
        return false;
    }
    return true;
}

static void assign_epoch(const char *newval, void *extra) {
    parse_epoch(newval, &snowflake_epoch_ms);
}

static void init_snowflake_shared(void *ptr) {
    SnowflakeShared *s = ptr;

    s->state = 0;
    s->epoch_ms = snowflake_epoch_ms;
    s->machine_id = (uint32) machine_id;
}

#if PG_VERSION_NUM >= 150000
static void idgenkit_shmem_request(void) {
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    RequestAddinShmemSpace(sizeof(SnowflakeShared));
}
#endif

static void idgenkit_shmem_startup(void) {
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    snowflake_shared = ShmemInitStruct("idgenkit snowflake state", sizeof(SnowflakeShared), &found);
    if (!found)
        init_snowflake_shared(snowflake_shared);
    LWLockRelease(AddinShmemInitLock);
}

/*
 * Returns the shared Snowflake struct, or NULL when it is unavailable
 * (PostgreSQL < 17 without shared_preload_libraries) and not required.
 */
static SnowflakeShared *get_snowflake_shared(bool required) {
    if (snowflake_shared == NULL) {
#if PG_VERSION_NUM >= 170000
        bool found;

        snowflake_shared = GetNamedDSMSegment("idgenkit_snowflake", sizeof(SnowflakeShared),
                                              init_snowflake_shared, &found);
#else
        if (required)
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("snowflake generation requires shared memory"),
                     errhint("Add idgenkit to shared_preload_libraries and restart the server.")));
#endif
    }
    return snowflake_shared;
}

static uint64 effective_epoch_ms(void) {
    SnowflakeShared *s = get_snowflake_shared(false);

    return s ? s->epoch_ms : snowflake_epoch_ms;
}

void _PG_init(void) {
    /* PGC_POSTMASTER settings can only be created while preloading. */
    GucContext context = process_shared_preload_libraries_in_progress ? PGC_POSTMASTER : PGC_SIGHUP;

    DefineCustomIntVariable("idgenkit.machine_id",
                            "Snowflake machine id (0-1023); must be unique per server in a cluster.",
                            "Captured at first use after server start.", &machine_id, 1, 0,
                            UID_SNOWFLAKE_MAX_MACHINE_ID, context, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("idgenkit.snowflake_epoch_ms",
                               "Custom Snowflake epoch in milliseconds since 1970-01-01 UTC.",
                               "Captured at first use after server start.", &snowflake_epoch_guc, "0",
                               context, 0, check_epoch, assign_epoch, NULL);
    DefineCustomStringVariable("idgenkit.relid_secret",
                               "Secret (at least 16 bytes) keying relative ID tags.",
                               "Visible to superusers only. Honoured only when idgenkit is in "
                               "shared_preload_libraries.",
                               &relid_secret, "", PGC_SUSET,
                               GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE, NULL, NULL,
                               NULL);
#if PG_VERSION_NUM >= 150000
    MarkGUCPrefixReserved("idgenkit");
#else
    EmitWarningsOnPlaceholders("idgenkit");
#endif

    if (!process_shared_preload_libraries_in_progress)
        return;
    preloaded = true;
#if PG_VERSION_NUM >= 150000
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = idgenkit_shmem_request;
#else
    RequestAddinShmemSpace(sizeof(uint64));
#endif
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = idgenkit_shmem_startup;
}

/* ---- helpers ---------------------------------------------------------------- */

static void check_rc(int rc) {
    if (rc != UID_OK)
        ereport(ERROR,
                (errcode(rc == UID_ERR_RANDOM ? ERRCODE_INTERNAL_ERROR : ERRCODE_DATA_EXCEPTION),
                 errmsg("idgenkit: %s", uid_strerror(rc))));
}

static TimestampTz unix_ms_to_timestamptz(uint64 ms) {
    return ((int64) ms - UNIX_EPOCH_TO_PG_EPOCH_MS) * 1000;
}

static int64 timestamptz_to_unix_ms(TimestampTz ts) {
    return ts / 1000 + UNIX_EPOCH_TO_PG_EPOCH_MS - (ts % 1000 < 0 ? 1 : 0);
}

static text *ulid_to_text(const uid_ulid *u) {
    char buf[UID_ULID_LEN];

    uid_ulid_encode(u, buf);
    return cstring_to_text_with_len(buf, UID_ULID_LEN);
}

static void text_to_ulid(text *t, uid_ulid *out) {
    if (uid_ulid_decode(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), out) != UID_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid ULID: \"%s\"", text_to_cstring(t))));
}

static pg_uuid_t *ulid_to_pg_uuid(const uid_ulid *u) {
    pg_uuid_t *uuid = palloc(sizeof(pg_uuid_t));

    memcpy(uuid->data, u->b, UUID_LEN);
    return uuid;
}

/* ---- ULID ------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(idgenkit_ulid_generate);
Datum idgenkit_ulid_generate(PG_FUNCTION_ARGS) {
    uid_ulid u;

    check_rc(uid_ulid_new(&u));
    PG_RETURN_TEXT_P(ulid_to_text(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_generate_monotonic);
Datum idgenkit_ulid_generate_monotonic(PG_FUNCTION_ARGS) {
    uid_ulid u;

    check_rc(uid_ulid_monotonic_next(&ulid_mono, &u));
    PG_RETURN_TEXT_P(ulid_to_text(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_generate_uuid);
Datum idgenkit_ulid_generate_uuid(PG_FUNCTION_ARGS) {
    uid_ulid u;

    check_rc(uid_ulid_new(&u));
    PG_RETURN_UUID_P(ulid_to_pg_uuid(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_to_uuid);
Datum idgenkit_ulid_to_uuid(PG_FUNCTION_ARGS) {
    uid_ulid u;

    text_to_ulid(PG_GETARG_TEXT_PP(0), &u);
    PG_RETURN_UUID_P(ulid_to_pg_uuid(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_from_uuid);
Datum idgenkit_ulid_from_uuid(PG_FUNCTION_ARGS) {
    pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
    uid_ulid u;

    memcpy(u.b, uuid->data, UUID_LEN);
    PG_RETURN_TEXT_P(ulid_to_text(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_timestamp);
Datum idgenkit_ulid_timestamp(PG_FUNCTION_ARGS) {
    uid_ulid u;

    text_to_ulid(PG_GETARG_TEXT_PP(0), &u);
    PG_RETURN_TIMESTAMPTZ(unix_ms_to_timestamptz(uid_ulid_timestamp(&u)));
}

PG_FUNCTION_INFO_V1(idgenkit_ulid_uuid_timestamp);
Datum idgenkit_ulid_uuid_timestamp(PG_FUNCTION_ARGS) {
    pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
    uid_ulid u;

    memcpy(u.b, uuid->data, UUID_LEN);
    PG_RETURN_TIMESTAMPTZ(unix_ms_to_timestamptz(uid_ulid_timestamp(&u)));
}

/* ---- UUIDv7 (v4 is the built-in gen_random_uuid()) ------------------------------ */

static pg_uuid_t *uid_uuid_to_pg(const uid_uuid *u) {
    pg_uuid_t *uuid = palloc(sizeof(pg_uuid_t));

    memcpy(uuid->data, u->b, UUID_LEN);
    return uuid;
}

PG_FUNCTION_INFO_V1(idgenkit_uuidv7_generate);
Datum idgenkit_uuidv7_generate(PG_FUNCTION_ARGS) {
    uid_uuid u;

    check_rc(uid_uuidv7(&u));
    PG_RETURN_UUID_P(uid_uuid_to_pg(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_uuidv7_generate_monotonic);
Datum idgenkit_uuidv7_generate_monotonic(PG_FUNCTION_ARGS) {
    uid_uuid u;

    check_rc(uid_uuidv7_monotonic_next(&uuidv7_mono, &u));
    PG_RETURN_UUID_P(uid_uuid_to_pg(&u));
}

PG_FUNCTION_INFO_V1(idgenkit_uuidv7_timestamp);
Datum idgenkit_uuidv7_timestamp(PG_FUNCTION_ARGS) {
    pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
    uid_uuid u;
    uint64 ms;

    memcpy(u.b, uuid->data, UUID_LEN);
    if (uid_uuidv7_timestamp(&u, &ms) != UID_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("uuid is version %u, not version 7", uid_uuid_version(&u))));
    PG_RETURN_TIMESTAMPTZ(unix_ms_to_timestamptz(ms));
}

/* ---- Relative ID ---------------------------------------------------------------- */

#define RELID_SALT_CACHE 256

static uid_relid_ctx relid_ctx;
static char *relid_ctx_secret = NULL;
static char relid_ctx_salt[RELID_SALT_CACHE];
static int relid_ctx_salt_len = -1;

/* The HMAC context for the current secret and this salt, reused while neither changes. */
static uid_relid_ctx *relid_context(text *salt_text) {
    const char *salt = VARDATA_ANY(salt_text);
    size_t salt_len = VARSIZE_ANY_EXHDR(salt_text);
    size_t secret_len;

    /* Without preloading, a configured secret is a placeholder any role can SHOW. */
    if (!preloaded)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("relative IDs require idgenkit in shared_preload_libraries"),
                 errhint("Add idgenkit to shared_preload_libraries so idgenkit.relid_secret is "
                         "registered as superuser-only, then restart the server.")));
    secret_len = relid_secret ? strlen(relid_secret) : 0;
    if (secret_len < UID_RELID_MIN_SECRET)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("idgenkit.relid_secret must be set to at least %d bytes",
                        UID_RELID_MIN_SECRET)));
    if (relid_ctx_salt_len >= 0 && (size_t) relid_ctx_salt_len == salt_len &&
        memcmp(relid_ctx_salt, salt, salt_len) == 0 && strcmp(relid_ctx_secret, relid_secret) == 0)
        return &relid_ctx;

    check_rc(uid_relid_ctx_init(&relid_ctx, (const uint8 *) relid_secret, secret_len, salt, salt_len));
    if (relid_ctx_secret == NULL || strcmp(relid_ctx_secret, relid_secret) != 0) {
        if (relid_ctx_secret) {
            explicit_bzero(relid_ctx_secret, strlen(relid_ctx_secret));
            pfree(relid_ctx_secret);
        }
        relid_ctx_secret = MemoryContextStrdup(TopMemoryContext, relid_secret);
    }
    if (salt_len <= RELID_SALT_CACHE) {
        memcpy(relid_ctx_salt, salt, salt_len);
        relid_ctx_salt_len = (int) salt_len;
    } else {
        relid_ctx_salt_len = -1;
    }
    return &relid_ctx;
}

static text *relid_to_text(const uid_relid *id) {
    char buf[UID_RELID_LEN];

    uid_relid_encode(id, buf);
    return cstring_to_text_with_len(buf, UID_RELID_LEN);
}

PG_FUNCTION_INFO_V1(idgenkit_relid_generate);
Datum idgenkit_relid_generate(PG_FUNCTION_ARGS) {
    text *key = PG_GETARG_TEXT_PP(0);
    uid_relid_ctx *ctx = relid_context(PG_GETARG_TEXT_PP(1));
    uid_relid id;

    check_rc(uid_relid_new(ctx, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key), &id));
    PG_RETURN_TEXT_P(relid_to_text(&id));
}

PG_FUNCTION_INFO_V1(idgenkit_relid_generate_monotonic);
Datum idgenkit_relid_generate_monotonic(PG_FUNCTION_ARGS) {
    text *key = PG_GETARG_TEXT_PP(0);
    uid_relid_ctx *ctx = relid_context(PG_GETARG_TEXT_PP(1));
    uid_relid id;

    check_rc(uid_relid_monotonic_next(&relid_mono, ctx, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key), &id));
    PG_RETURN_TEXT_P(relid_to_text(&id));
}

PG_FUNCTION_INFO_V1(idgenkit_relid_tag);
Datum idgenkit_relid_tag(PG_FUNCTION_ARGS) {
    text *key = PG_GETARG_TEXT_PP(0);
    uid_relid_ctx *ctx = relid_context(PG_GETARG_TEXT_PP(1));
    char buf[UID_RELID_TAG_LEN];

    uid_relid_tag_encode(uid_relid_tag(ctx, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key)), buf);
    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, UID_RELID_TAG_LEN));
}

PG_FUNCTION_INFO_V1(idgenkit_relid_timestamp);
Datum idgenkit_relid_timestamp(PG_FUNCTION_ARGS) {
    text *t = PG_GETARG_TEXT_PP(0);
    uid_relid id;

    if (uid_relid_decode(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &id) != UID_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid relative ID: \"%s\"", text_to_cstring(t))));
    PG_RETURN_TIMESTAMPTZ(unix_ms_to_timestamptz(id.timestamp_ms));
}

/* ---- Snowflake ---------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(idgenkit_snowflake_generate);
Datum idgenkit_snowflake_generate(PG_FUNCTION_ARGS) {
    SnowflakeShared *s = get_snowflake_shared(true);
    uint64 id;

    check_rc(uid_snowflake_next(&s->state, s->machine_id, s->epoch_ms, &id));
    if (id > (uint64) PG_INT64_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("snowflake id exceeds the bigint range"),
                 errhint("Set idgenkit.snowflake_epoch_ms to a more recent epoch.")));
    PG_RETURN_INT64((int64) id);
}

PG_FUNCTION_INFO_V1(idgenkit_snowflake_timestamp);
Datum idgenkit_snowflake_timestamp(PG_FUNCTION_ARGS) {
    uint64 ts;
    uint32 mid, seq;

    uid_snowflake_parse((uint64) PG_GETARG_INT64(0), effective_epoch_ms(), &ts, &mid, &seq);
    PG_RETURN_TIMESTAMPTZ(unix_ms_to_timestamptz(ts));
}

PG_FUNCTION_INFO_V1(idgenkit_snowflake_machine_id);
Datum idgenkit_snowflake_machine_id(PG_FUNCTION_ARGS) {
    uint64 ts;
    uint32 mid, seq;

    uid_snowflake_parse((uint64) PG_GETARG_INT64(0), 0, &ts, &mid, &seq);
    PG_RETURN_INT32((int32) mid);
}

PG_FUNCTION_INFO_V1(idgenkit_snowflake_sequence);
Datum idgenkit_snowflake_sequence(PG_FUNCTION_ARGS) {
    uint64 ts;
    uint32 mid, seq;

    uid_snowflake_parse((uint64) PG_GETARG_INT64(0), 0, &ts, &mid, &seq);
    PG_RETURN_INT32((int32) seq);
}

PG_FUNCTION_INFO_V1(idgenkit_snowflake_from_timestamp);
Datum idgenkit_snowflake_from_timestamp(PG_FUNCTION_ARGS) {
    int64 ms = timestamptz_to_unix_ms(PG_GETARG_TIMESTAMPTZ(0));
    uint64 id;

    if (ms < 0 || uid_snowflake_compose((uint64) ms, 0, 0, effective_epoch_ms(), &id) != UID_OK ||
        id > (uint64) PG_INT64_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("timestamp is outside the snowflake range for the configured epoch")));
    PG_RETURN_INT64((int64) id);
}

/* ---- Nano ID ------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(idgenkit_nanoid_generate);
Datum idgenkit_nanoid_generate(PG_FUNCTION_ARGS) {
    int32 size = PG_GETARG_INT32(0);
    text *alphabet = PG_GETARG_TEXT_PP(1);
    text *result;
    int rc;

    if (size < 1 || size > NANOID_MAX_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("nanoid size must be between 1 and %d", NANOID_MAX_SIZE)));
    result = palloc(VARHDRSZ + size);
    SET_VARSIZE(result, VARHDRSZ + size);
    rc = uid_nanoid(VARDATA(result), (size_t) size, VARDATA_ANY(alphabet), VARSIZE_ANY_EXHDR(alphabet));
    if (rc == UID_ERR_INVALID)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("nanoid alphabet must contain between 1 and 256 ASCII symbols")));
    check_rc(rc);
    PG_RETURN_TEXT_P(result);
}
