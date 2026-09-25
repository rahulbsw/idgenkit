/*
 * MySQL / MariaDB loadable functions (UDFs) for ULID, UUIDv4/v7, Snowflake and
 * Nano ID. See install.sql for the CREATE FUNCTION statements.
 *
 * Concurrency: MySQL runs UDFs concurrently from many connection threads.
 *  - Snowflake state is one process-wide word updated with atomic CAS, so IDs
 *    are unique across all connections of the server.
 *  - Monotonic ULID and UUIDv7 state is thread-local: monotonic per connection
 *    thread, globally unique through their 80 / 74 random bits.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>

#include "idgenkit.h"

#define NANOID_MAX_SIZE 1024

static uint64_t snowflake_state = 0;
static __thread uid_ulid_monotonic ulid_mono;
static __thread uid_uuidv7_monotonic uuidv7_mono;

/* Mark the function non-deterministic so it is evaluated once per row. */
static void init_volatile(UDF_INIT *initid, bool maybe_null, unsigned long max_length) {
    initid->const_item = 0;
    initid->maybe_null = maybe_null;
    initid->max_length = max_length;
}

static bool require_args(UDF_ARGS *args, char *message, unsigned min, unsigned max, const char *usage) {
    if (args->arg_count < min || args->arg_count > max) {
        snprintf(message, MYSQL_ERRMSG_SIZE, "%s", usage);
        return true;
    }
    return false;
}

static bool arg_int(UDF_ARGS *args, unsigned i, long long *out) {
    if (i >= args->arg_count || args->args[i] == NULL)
        return false;
    *out = *(long long *)args->args[i];
    return true;
}

/* ---- ULID ----------------------------------------------------------------- */

bool ulid_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, UID_ULID_LEN);
    return require_args(args, message, 0, 0, "ulid_generate() takes no arguments");
}

char *ulid_generate(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                    unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)args, (void)is_null;
    uid_ulid u;
    if (uid_ulid_new(&u) != UID_OK) {
        *error = 1;
        return NULL;
    }
    uid_ulid_encode(&u, result);
    *length = UID_ULID_LEN;
    return result;
}

bool ulid_generate_monotonic_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, UID_ULID_LEN);
    return require_args(args, message, 0, 0, "ulid_generate_monotonic() takes no arguments");
}

char *ulid_generate_monotonic(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                              unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)args, (void)is_null;
    uid_ulid u;
    if (uid_ulid_monotonic_next(&ulid_mono, &u) != UID_OK) {
        *error = 1;
        return NULL;
    }
    uid_ulid_encode(&u, result);
    *length = UID_ULID_LEN;
    return result;
}

static bool one_string_arg(UDF_INIT *initid, UDF_ARGS *args, char *message, unsigned long max_length,
                           const char *usage) {
    initid->maybe_null = true;
    initid->max_length = max_length;
    if (require_args(args, message, 1, 1, usage))
        return true;
    args->arg_type[0] = STRING_RESULT;
    return false;
}

bool ulid_timestamp_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return one_string_arg(initid, args, message, 21, "usage: ulid_timestamp(ulid)");
}

long long ulid_timestamp(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null, unsigned char *error) {
    (void)initid;
    uid_ulid u;
    if (args->args[0] == NULL) {
        *is_null = 1;
        return 0;
    }
    if (uid_ulid_decode(args->args[0], args->lengths[0], &u) != UID_OK) {
        *error = 1;
        return 0;
    }
    return (long long)uid_ulid_timestamp(&u);
}

bool ulid_to_bin_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return one_string_arg(initid, args, message, 16, "usage: ulid_to_bin(ulid)");
}

char *ulid_to_bin(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                  unsigned char *is_null, unsigned char *error) {
    (void)initid;
    uid_ulid u;
    if (args->args[0] == NULL) {
        *is_null = 1;
        return NULL;
    }
    if (uid_ulid_decode(args->args[0], args->lengths[0], &u) != UID_OK) {
        *error = 1;
        return NULL;
    }
    memcpy(result, u.b, 16);
    *length = 16;
    return result;
}

bool bin_to_ulid_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return one_string_arg(initid, args, message, UID_ULID_LEN, "usage: bin_to_ulid(binary16)");
}

char *bin_to_ulid(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                  unsigned char *is_null, unsigned char *error) {
    (void)initid;
    if (args->args[0] == NULL) {
        *is_null = 1;
        return NULL;
    }
    if (args->lengths[0] != 16) {
        *error = 1;
        return NULL;
    }
    uid_ulid u;
    memcpy(u.b, args->args[0], 16);
    uid_ulid_encode(&u, result);
    *length = UID_ULID_LEN;
    return result;
}

/* ---- UUIDv4 / UUIDv7 ---------------------------------------------------------- */

static char *uuid_result(int rc, const uid_uuid *u, char *result, unsigned long *length,
                         unsigned char *error) {
    if (rc != UID_OK) {
        *error = 1;
        return NULL;
    }
    uid_uuid_encode(u, result);
    *length = UID_UUID_LEN;
    return result;
}

bool uuidv4_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, UID_UUID_LEN);
    return require_args(args, message, 0, 0, "uuidv4_generate() takes no arguments");
}

char *uuidv4_generate(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                      unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)args, (void)is_null;
    uid_uuid u;
    return uuid_result(uid_uuidv4(&u), &u, result, length, error);
}

bool uuidv7_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, UID_UUID_LEN);
    return require_args(args, message, 0, 0, "uuidv7_generate() takes no arguments");
}

char *uuidv7_generate(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                      unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)args, (void)is_null;
    uid_uuid u;
    return uuid_result(uid_uuidv7(&u), &u, result, length, error);
}

bool uuidv7_generate_monotonic_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, UID_UUID_LEN);
    return require_args(args, message, 0, 0, "uuidv7_generate_monotonic() takes no arguments");
}

char *uuidv7_generate_monotonic(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                                unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)args, (void)is_null;
    uid_uuid u;
    return uuid_result(uid_uuidv7_monotonic_next(&uuidv7_mono, &u), &u, result, length, error);
}

bool uuidv7_timestamp_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return one_string_arg(initid, args, message, 21, "usage: uuidv7_timestamp(uuid text or UUID_TO_BIN value)");
}

long long uuidv7_timestamp(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null, unsigned char *error) {
    (void)initid;
    uid_uuid u;
    uint64_t ms;
    if (args->args[0] == NULL) {
        *is_null = 1;
        return 0;
    }
    if (args->lengths[0] == 16)
        memcpy(u.b, args->args[0], 16);
    else if (uid_uuid_decode(args->args[0], args->lengths[0], &u) != UID_OK)
        *error = 1;
    if (*error || uid_uuidv7_timestamp(&u, &ms) != UID_OK) {
        *error = 1;
        return 0;
    }
    return (long long)ms;
}

/* ---- Snowflake -------------------------------------------------------------- */

bool snowflake_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, false, 20);
    if (require_args(args, message, 0, 2, "usage: snowflake_generate([machine_id [, epoch_ms]])"))
        return true;
    for (unsigned i = 0; i < args->arg_count; i++)
        args->arg_type[i] = INT_RESULT;
    return false;
}

long long snowflake_generate(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null,
                             unsigned char *error) {
    (void)initid, (void)is_null;
    long long machine_id = 1, epoch_ms = 0;
    arg_int(args, 0, &machine_id);
    arg_int(args, 1, &epoch_ms);
    uint64_t id;
    if (machine_id < 0 || machine_id > (long long)UID_SNOWFLAKE_MAX_MACHINE_ID || epoch_ms < 0 ||
        uid_snowflake_next(&snowflake_state, (uint32_t)machine_id, (uint64_t)epoch_ms, &id) != UID_OK ||
        id > (uint64_t)INT64_MAX) {
        *error = 1;
        return 0;
    }
    return (long long)id;
}

static bool snowflake_part_init(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *usage) {
    initid->maybe_null = true;
    if (require_args(args, message, 1, 2, usage))
        return true;
    for (unsigned i = 0; i < args->arg_count; i++)
        args->arg_type[i] = INT_RESULT;
    return false;
}

enum part { PART_TIMESTAMP, PART_MACHINE_ID, PART_SEQUENCE };

static long long snowflake_part(UDF_ARGS *args, unsigned char *is_null, enum part which) {
    long long id, epoch_ms = 0;
    if (!arg_int(args, 0, &id)) {
        *is_null = 1;
        return 0;
    }
    arg_int(args, 1, &epoch_ms);
    uint64_t ts;
    uint32_t mid, seq;
    uid_snowflake_parse((uint64_t)id, (uint64_t)epoch_ms, &ts, &mid, &seq);
    switch (which) {
    case PART_TIMESTAMP:
        return (long long)ts;
    case PART_MACHINE_ID:
        return mid;
    case PART_SEQUENCE:
        return seq;
    }
    return 0;
}

bool snowflake_timestamp_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return snowflake_part_init(initid, args, message, "usage: snowflake_timestamp(id [, epoch_ms])");
}

long long snowflake_timestamp(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)error;
    return snowflake_part(args, is_null, PART_TIMESTAMP);
}

bool snowflake_machine_id_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return snowflake_part_init(initid, args, message, "usage: snowflake_machine_id(id)");
}

long long snowflake_machine_id(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)error;
    return snowflake_part(args, is_null, PART_MACHINE_ID);
}

bool snowflake_sequence_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    return snowflake_part_init(initid, args, message, "usage: snowflake_sequence(id)");
}

long long snowflake_sequence(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null, unsigned char *error) {
    (void)initid, (void)error;
    return snowflake_part(args, is_null, PART_SEQUENCE);
}

/* ---- Nano ID ---------------------------------------------------------------- */

bool nanoid_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    init_volatile(initid, true, NANOID_MAX_SIZE);
    if (require_args(args, message, 0, 2, "usage: nanoid_generate([size [, alphabet]])"))
        return true;
    if (args->arg_count >= 1)
        args->arg_type[0] = INT_RESULT;
    if (args->arg_count == 2)
        args->arg_type[1] = STRING_RESULT;
    initid->ptr = malloc(NANOID_MAX_SIZE);
    if (initid->ptr == NULL) {
        snprintf(message, MYSQL_ERRMSG_SIZE, "nanoid_generate: out of memory");
        return true;
    }
    return false;
}

void nanoid_generate_deinit(UDF_INIT *initid) {
    free(initid->ptr);
}

char *nanoid_generate(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length,
                      unsigned char *is_null, unsigned char *error) {
    (void)result, (void)is_null;
    long long size = UID_NANOID_DEFAULT_SIZE;
    const char *alphabet = uid_nanoid_url_alphabet;
    size_t alen = 64;
    if (args->arg_count >= 1 && !arg_int(args, 0, &size)) {
        *is_null = 1;
        return NULL;
    }
    if (args->arg_count == 2) {
        if (args->args[1] == NULL) {
            *is_null = 1;
            return NULL;
        }
        alphabet = args->args[1];
        alen = args->lengths[1];
    }
    if (size < 1 || size > NANOID_MAX_SIZE ||
        uid_nanoid(initid->ptr, (size_t)size, alphabet, alen) != UID_OK) {
        *error = 1;
        return NULL;
    }
    *length = (unsigned long)size;
    return initid->ptr;
}
