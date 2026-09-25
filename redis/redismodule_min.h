/*
 * Minimal binding to the Redis Modules API (stable since Redis 4.0), covering
 * only the calls this module uses. Redis passes a lookup function in the
 * first word of the context; every API symbol is resolved through it, which
 * is exactly what the official redismodule.h does.
 */
#ifndef REDISMODULE_MIN_H
#define REDISMODULE_MIN_H

#include <stddef.h>

#define REDISMODULE_OK 0
#define REDISMODULE_ERR 1
#define REDISMODULE_APIVER_1 1

typedef struct RedisModuleCtx RedisModuleCtx;
typedef struct RedisModuleString RedisModuleString;
typedef int (*RedisModuleCmdFunc)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);

#define REDISMODULE_API_FUNC(x) (*x)

static int REDISMODULE_API_FUNC(RedisModule_GetApi)(const char *, void *);
static void REDISMODULE_API_FUNC(RedisModule_SetModuleAttribs)(RedisModuleCtx *, const char *, int, int);
static int REDISMODULE_API_FUNC(RedisModule_IsModuleNameBusy)(const char *);
static int REDISMODULE_API_FUNC(RedisModule_CreateCommand)(RedisModuleCtx *, const char *, RedisModuleCmdFunc,
                                                           const char *, int, int, int);
static int REDISMODULE_API_FUNC(RedisModule_WrongArity)(RedisModuleCtx *);
static int REDISMODULE_API_FUNC(RedisModule_ReplyWithLongLong)(RedisModuleCtx *, long long);
static int REDISMODULE_API_FUNC(RedisModule_ReplyWithError)(RedisModuleCtx *, const char *);
static int REDISMODULE_API_FUNC(RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *, const char *, size_t);
static int REDISMODULE_API_FUNC(RedisModule_ReplyWithArray)(RedisModuleCtx *, long);
static const char *REDISMODULE_API_FUNC(RedisModule_StringPtrLen)(const RedisModuleString *, size_t *);
static int REDISMODULE_API_FUNC(RedisModule_StringToLongLong)(const RedisModuleString *, long long *);

#define REDISMODULE_GET_API(name) RedisModule_GetApi("RedisModule_" #name, ((void **)&RedisModule_##name))

static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    void *getapifuncptr = ((void **)ctx)[0];
    RedisModule_GetApi = (int (*)(const char *, void *))(unsigned long)getapifuncptr;
    if (REDISMODULE_GET_API(SetModuleAttribs) != REDISMODULE_OK ||
        REDISMODULE_GET_API(CreateCommand) != REDISMODULE_OK ||
        REDISMODULE_GET_API(WrongArity) != REDISMODULE_OK ||
        REDISMODULE_GET_API(ReplyWithLongLong) != REDISMODULE_OK ||
        REDISMODULE_GET_API(ReplyWithError) != REDISMODULE_OK ||
        REDISMODULE_GET_API(ReplyWithStringBuffer) != REDISMODULE_OK ||
        REDISMODULE_GET_API(ReplyWithArray) != REDISMODULE_OK ||
        REDISMODULE_GET_API(StringPtrLen) != REDISMODULE_OK ||
        REDISMODULE_GET_API(StringToLongLong) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    /* Optional (Redis >= 6); absent on older servers. */
    REDISMODULE_GET_API(IsModuleNameBusy);
    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name))
        return REDISMODULE_ERR;
    RedisModule_SetModuleAttribs(ctx, name, ver, apiver);
    return REDISMODULE_OK;
}

#endif
