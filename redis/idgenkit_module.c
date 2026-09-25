/*
 * Redis module exposing ULID, UUIDv4/v7, Snowflake and Nano ID generation.
 *
 *   loadmodule /path/idgenkit.so [MACHINE_ID <0-1023>] [EPOCH_MS <ms>]
 *
 * Commands (all keyless, O(1)):
 *   ULID.GENERATE                     -> 26-char ULID
 *   ULID.MONOTONIC                    -> strictly increasing ULID
 *   ULID.TIME <ulid>                  -> embedded Unix time in ms
 *   UUIDV4.GENERATE                   -> 36-char random UUID
 *   UUIDV7.GENERATE                   -> 36-char time-ordered UUID
 *   UUIDV7.MONOTONIC                  -> strictly increasing UUIDv7
 *   UUIDV7.TIME <uuid>                -> embedded Unix time in ms
 *   SNOWFLAKE.GENERATE                -> 64-bit integer id
 *   SNOWFLAKE.PARSE <id>              -> [timestamp_ms, machine_id, sequence]
 *   NANOID.GENERATE [size [alphabet]] -> Nano ID (default size 21, URL alphabet)
 *
 * Commands execute on the Redis main thread, so the monotonic and Snowflake
 * state need no extra locking.
 */
#include "redismodule_min.h"
#include "idgenkit.h"

#include <string.h>
#include <strings.h>

#define NANOID_MAX_SIZE 1024

static uint32_t machine_id = 1;
static uint64_t epoch_ms = 0;
static uint64_t snowflake_state = 0;
static uid_ulid_monotonic ulid_mono;
static uid_uuidv7_monotonic uuidv7_mono;

static int reply_error(RedisModuleCtx *ctx, int rc) {
    switch (rc) {
    case UID_ERR_RANGE:
        return RedisModule_ReplyWithError(ctx, "ERR value out of range (check clock and EPOCH_MS)");
    case UID_ERR_OVERFLOW:
        return RedisModule_ReplyWithError(ctx, "ERR monotonic overflow within one millisecond");
    case UID_ERR_INVALID:
        return RedisModule_ReplyWithError(ctx, "ERR invalid argument");
    default:
        return RedisModule_ReplyWithError(ctx, "ERR random source failure");
    }
}

static int reply_ulid(RedisModuleCtx *ctx, const uid_ulid *u) {
    char text[UID_ULID_LEN];
    uid_ulid_encode(u, text);
    return RedisModule_ReplyWithStringBuffer(ctx, text, sizeof text);
}

static int UlidGenerate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uid_ulid u;
    int rc = uid_ulid_new(&u);
    return rc == UID_OK ? reply_ulid(ctx, &u) : reply_error(ctx, rc);
}

static int UlidMonotonic(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uid_ulid u;
    int rc = uid_ulid_monotonic_next(&ulid_mono, &u);
    return rc == UID_OK ? reply_ulid(ctx, &u) : reply_error(ctx, rc);
}

static int UlidTime(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
    size_t len;
    const char *s = RedisModule_StringPtrLen(argv[1], &len);
    uid_ulid u;
    if (uid_ulid_decode(s, len, &u) != UID_OK)
        return RedisModule_ReplyWithError(ctx, "ERR invalid ULID");
    return RedisModule_ReplyWithLongLong(ctx, (long long)uid_ulid_timestamp(&u));
}

static int reply_uuid(RedisModuleCtx *ctx, int rc, const uid_uuid *u) {
    char text[UID_UUID_LEN];
    if (rc != UID_OK)
        return reply_error(ctx, rc);
    uid_uuid_encode(u, text);
    return RedisModule_ReplyWithStringBuffer(ctx, text, sizeof text);
}

static int Uuidv4Generate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uid_uuid u;
    return reply_uuid(ctx, uid_uuidv4(&u), &u);
}

static int Uuidv7Generate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uid_uuid u;
    return reply_uuid(ctx, uid_uuidv7(&u), &u);
}

static int Uuidv7Monotonic(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uid_uuid u;
    return reply_uuid(ctx, uid_uuidv7_monotonic_next(&uuidv7_mono, &u), &u);
}

static int Uuidv7Time(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
    size_t len;
    const char *s = RedisModule_StringPtrLen(argv[1], &len);
    uid_uuid u;
    uint64_t ms;
    if (uid_uuid_decode(s, len, &u) != UID_OK)
        return RedisModule_ReplyWithError(ctx, "ERR invalid UUID");
    if (uid_uuidv7_timestamp(&u, &ms) != UID_OK)
        return RedisModule_ReplyWithError(ctx, "ERR not a version 7 UUID");
    return RedisModule_ReplyWithLongLong(ctx, (long long)ms);
}

static int SnowflakeGenerate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1)
        return RedisModule_WrongArity(ctx);
    uint64_t id;
    int rc = uid_snowflake_next(&snowflake_state, machine_id, epoch_ms, &id);
    if (rc != UID_OK)
        return reply_error(ctx, rc);
    if (id > (uint64_t)INT64_MAX)
        return RedisModule_ReplyWithError(ctx, "ERR id exceeds signed 64-bit range; configure a later EPOCH_MS");
    return RedisModule_ReplyWithLongLong(ctx, (long long)id);
}

static int SnowflakeParse(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);
    long long id;
    if (RedisModule_StringToLongLong(argv[1], &id) != REDISMODULE_OK || id < 0)
        return RedisModule_ReplyWithError(ctx, "ERR id must be a non-negative integer");
    uint64_t ts;
    uint32_t mid, seq;
    uid_snowflake_parse((uint64_t)id, epoch_ms, &ts, &mid, &seq);
    RedisModule_ReplyWithArray(ctx, 3);
    RedisModule_ReplyWithLongLong(ctx, (long long)ts);
    RedisModule_ReplyWithLongLong(ctx, mid);
    return RedisModule_ReplyWithLongLong(ctx, seq);
}

static int NanoidGenerate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc > 3)
        return RedisModule_WrongArity(ctx);
    long long size = UID_NANOID_DEFAULT_SIZE;
    if (argc >= 2 && (RedisModule_StringToLongLong(argv[1], &size) != REDISMODULE_OK || size < 1 ||
                      size > NANOID_MAX_SIZE))
        return RedisModule_ReplyWithError(ctx, "ERR size must be an integer between 1 and 1024");
    const char *alphabet = uid_nanoid_url_alphabet;
    size_t alen = 64;
    if (argc == 3)
        alphabet = RedisModule_StringPtrLen(argv[2], &alen);
    char out[NANOID_MAX_SIZE];
    int rc = uid_nanoid(out, (size_t)size, alphabet, alen);
    if (rc == UID_ERR_INVALID)
        return RedisModule_ReplyWithError(ctx, "ERR alphabet must contain 1 to 256 ASCII symbols");
    if (rc != UID_OK)
        return reply_error(ctx, rc);
    return RedisModule_ReplyWithStringBuffer(ctx, out, (size_t)size);
}

static int parse_args(RedisModuleString **argv, int argc) {
    for (int i = 0; i + 1 < argc; i += 2) {
        size_t len;
        const char *key = RedisModule_StringPtrLen(argv[i], &len);
        long long v;
        if (RedisModule_StringToLongLong(argv[i + 1], &v) != REDISMODULE_OK)
            return REDISMODULE_ERR;
        if (strcasecmp(key, "MACHINE_ID") == 0 && v >= 0 && v <= (long long)UID_SNOWFLAKE_MAX_MACHINE_ID)
            machine_id = (uint32_t)v;
        else if (strcasecmp(key, "EPOCH_MS") == 0 && v >= 0)
            epoch_ms = (uint64_t)v;
        else
            return REDISMODULE_ERR;
    }
    return argc % 2 == 0 ? REDISMODULE_OK : REDISMODULE_ERR;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "idgenkit", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (parse_args(argv, argc) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    struct {
        const char *name;
        RedisModuleCmdFunc fn;
    } cmds[] = {
        {"ulid.generate", UlidGenerate},         {"ulid.monotonic", UlidMonotonic},
        {"ulid.time", UlidTime},                 {"snowflake.generate", SnowflakeGenerate},
        {"snowflake.parse", SnowflakeParse},     {"nanoid.generate", NanoidGenerate},
        {"uuidv4.generate", Uuidv4Generate},     {"uuidv7.generate", Uuidv7Generate},
        {"uuidv7.monotonic", Uuidv7Monotonic},   {"uuidv7.time", Uuidv7Time},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (RedisModule_CreateCommand(ctx, cmds[i].name, cmds[i].fn, "fast", 0, 0, 0) != REDISMODULE_OK)
            return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
