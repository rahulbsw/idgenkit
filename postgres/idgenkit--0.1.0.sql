\echo Use "CREATE EXTENSION idgenkit" to load this file. \quit

-- ULID -------------------------------------------------------------------------

CREATE FUNCTION ulid_generate() RETURNS text
AS 'MODULE_PATHNAME', 'idgenkit_ulid_generate'
LANGUAGE C VOLATILE PARALLEL SAFE;
COMMENT ON FUNCTION ulid_generate() IS 'Random ULID (48-bit ms timestamp + 80 random bits) as 26-char Crockford base32';

-- Monotonic state is per backend, so keep it out of parallel workers.
CREATE FUNCTION ulid_generate_monotonic() RETURNS text
AS 'MODULE_PATHNAME', 'idgenkit_ulid_generate_monotonic'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION ulid_generate_monotonic() IS 'ULID that strictly increases within the current session';

CREATE FUNCTION ulid_generate_uuid() RETURNS uuid
AS 'MODULE_PATHNAME', 'idgenkit_ulid_generate_uuid'
LANGUAGE C VOLATILE PARALLEL SAFE;
COMMENT ON FUNCTION ulid_generate_uuid() IS 'Random ULID in the 16-byte uuid type (sorts by creation time)';

CREATE FUNCTION ulid_to_uuid(text) RETURNS uuid
AS 'MODULE_PATHNAME', 'idgenkit_ulid_to_uuid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ulid_from_uuid(uuid) RETURNS text
AS 'MODULE_PATHNAME', 'idgenkit_ulid_from_uuid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ulid_timestamp(text) RETURNS timestamptz
AS 'MODULE_PATHNAME', 'idgenkit_ulid_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ulid_timestamp(uuid) RETURNS timestamptz
AS 'MODULE_PATHNAME', 'idgenkit_ulid_uuid_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- UUIDv7 -----------------------------------------------------------------------
-- For UUIDv4 use the built-in gen_random_uuid().

CREATE FUNCTION uuidv7_generate() RETURNS uuid
AS 'MODULE_PATHNAME', 'idgenkit_uuidv7_generate'
LANGUAGE C VOLATILE PARALLEL SAFE;
COMMENT ON FUNCTION uuidv7_generate() IS 'RFC 9562 UUIDv7: 48-bit ms timestamp + 74 random bits';

CREATE FUNCTION uuidv7_generate_monotonic() RETURNS uuid
AS 'MODULE_PATHNAME', 'idgenkit_uuidv7_generate_monotonic'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION uuidv7_generate_monotonic() IS 'UUIDv7 that strictly increases within the current session';

CREATE FUNCTION uuidv7_timestamp(uuid) RETURNS timestamptz
AS 'MODULE_PATHNAME', 'idgenkit_uuidv7_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION uuidv7_timestamp(uuid) IS 'Creation time of a UUIDv7; error for other versions';

-- Snowflake --------------------------------------------------------------------
-- Configured with idgenkit.machine_id and idgenkit.snowflake_epoch_ms (server start only).

CREATE FUNCTION snowflake_generate() RETURNS bigint
AS 'MODULE_PATHNAME', 'idgenkit_snowflake_generate'
LANGUAGE C VOLATILE PARALLEL SAFE;
COMMENT ON FUNCTION snowflake_generate() IS '64-bit id: 42-bit ms since epoch | 10-bit machine id | 12-bit sequence';

CREATE FUNCTION snowflake_timestamp(bigint) RETURNS timestamptz
AS 'MODULE_PATHNAME', 'idgenkit_snowflake_timestamp'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION snowflake_machine_id(bigint) RETURNS integer
AS 'MODULE_PATHNAME', 'idgenkit_snowflake_machine_id'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION snowflake_sequence(bigint) RETURNS integer
AS 'MODULE_PATHNAME', 'idgenkit_snowflake_sequence'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION snowflake_from_timestamp(timestamptz) RETURNS bigint
AS 'MODULE_PATHNAME', 'idgenkit_snowflake_from_timestamp'
LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION snowflake_from_timestamp(timestamptz) IS 'Smallest snowflake id for a timestamp, for range scans';

-- Nano ID ----------------------------------------------------------------------

CREATE FUNCTION nanoid_generate(
    size integer DEFAULT 21,
    alphabet text DEFAULT 'useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict'
) RETURNS text
AS 'MODULE_PATHNAME', 'idgenkit_nanoid_generate'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION nanoid_generate(integer, text) IS 'Nano ID (default 21 URL-safe chars); alphabet of 1-256 ASCII symbols';
