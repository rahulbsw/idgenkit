-- Run by test.sh against a server started with
--   idgenkit.machine_id = 7, idgenkit.snowflake_epoch_ms = 1288834974657
CREATE EXTENSION idgenkit;
SET timezone = 'UTC';

-- ULID
SELECT ulid_generate() ~ '^[0-7][0-9A-HJKMNP-TV-Z]{25}$' AS ulid_format;
SELECT count(DISTINCT ulid_generate()) AS distinct_ulids FROM generate_series(1, 10000);
SELECT abs(extract(epoch FROM ulid_timestamp(ulid_generate()) - now())) < 5 AS ulid_time_is_now;
SELECT ulid_timestamp('01ARYZ6S41TSV4RRFFQ69G5FAV') AS anchor_time;
SELECT ulid_timestamp('01aryz6s41tsv4rrffq69g5fav') = ulid_timestamp('01ARYZ6S41TSV4RRFFQ69G5FAV') AS case_insensitive;
SELECT ulid_to_uuid('01ARYZ6S41TSV4RRFFQ69G5FAV') AS as_uuid;
SELECT ulid_from_uuid(ulid_to_uuid('01ARYZ6S41TSV4RRFFQ69G5FAV')) AS round_trip;
SELECT ulid_timestamp(ulid_to_uuid('01ARYZ6S41TSV4RRFFQ69G5FAV')) AS uuid_time;
SELECT ulid_to_uuid('00000000000000000000000000') AS zero, ulid_to_uuid('7ZZZZZZZZZZZZZZZZZZZZZZZZZ') AS max;
SELECT ulid_timestamp('8ZZZZZZZZZZZZZZZZZZZZZZZZZ');
SELECT ulid_timestamp('01ARYZ6S41TSV4RRFFQ69G5FAU');
SELECT ulid_timestamp('short');
SELECT ulid_timestamp(NULL::text) IS NULL AS strict_null;

-- monotonic ULIDs strictly increase (compare bytewise via COLLATE "C")
WITH g AS (SELECT n, ulid_generate_monotonic() AS u FROM generate_series(1, 20000) n)
SELECT bool_and(u COLLATE "C" > prev COLLATE "C") AS monotonic
FROM (SELECT u, lag(u) OVER (ORDER BY n) AS prev FROM g) s WHERE prev IS NOT NULL;

-- uuid form sorts in the same order as the text form
WITH g AS (SELECT ulid_generate() AS u FROM generate_series(1, 5000))
SELECT bool_and(a.rn = b.rn) AS same_order
FROM (SELECT u, row_number() OVER (ORDER BY u COLLATE "C") AS rn FROM g) a
JOIN (SELECT u, row_number() OVER (ORDER BY ulid_to_uuid(u)) AS rn FROM g) b USING (u);
SELECT count(DISTINCT ulid_generate_uuid()) AS distinct_uuids FROM generate_series(1, 10000);

-- UUIDv7
SELECT uuidv7_generate()::text ~ '^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' AS uuidv7_format;
SELECT count(DISTINCT uuidv7_generate()) AS distinct_uuidv7s FROM generate_series(1, 10000);
SELECT abs(extract(epoch FROM uuidv7_timestamp(uuidv7_generate()) - now())) < 5 AS uuidv7_time_is_now;
-- RFC 9562 appendix A.6
SELECT uuidv7_timestamp('017f22e2-79b0-7cc3-98c4-dc0c0c07398f') AS rfc_time;
WITH g AS (SELECT n, uuidv7_generate_monotonic() AS u FROM generate_series(1, 20000) n)
SELECT bool_and(u > prev) AS uuidv7_monotonic
FROM (SELECT u, lag(u) OVER (ORDER BY n) AS prev FROM g) s WHERE prev IS NOT NULL;
SELECT uuidv7_timestamp(gen_random_uuid());
SELECT uuidv7_timestamp(NULL) IS NULL AS uuidv7_strict_null;

-- Relative ID (test-only secret; vectors from testdata/relid_tag.txt)
SET idgenkit.relid_secret = '0123456789abcdef';
SELECT relid_tag('customer-42') AS tag, relid_tag('customer-42', 'orders') AS orders_tag;
SELECT relid_generate('customer-42', 'orders') ~ '^5731MX-[0-7][0-9A-HJKMNP-TV-Z]{9}-[0-9A-HJKMNP-TV-Z]{10}$' AS relid_format;
SELECT count(DISTINCT relid_generate('k')) AS distinct_relids FROM generate_series(1, 10000);
SELECT abs(extract(epoch FROM relid_timestamp(relid_generate('k')) - now())) < 5 AS relid_time_is_now;
-- from testdata/relid_monotonic.txt
SELECT relid_timestamp('0AQKFF-01ARYZ6S41-RJ6HB7H6NW') AS relid_anchor_time;
SELECT relid_timestamp('0aqkff01aryz6s41rj6hb7h6nw') = relid_timestamp('0AQKFF-01ARYZ6S41-RJ6HB7H6NW') AS bare_lowercase;
WITH g AS (SELECT n, relid_generate_monotonic(CASE WHEN n % 3 = 0 THEN 'a' ELSE 'b' END) AS id
           FROM generate_series(1, 20000) n)
SELECT bool_and(id COLLATE "C" > prev COLLATE "C") AS relid_monotonic_per_key
FROM (SELECT id, lag(id) OVER (PARTITION BY left(id, 6) ORDER BY n) AS prev FROM g) s WHERE prev IS NOT NULL;
SELECT count(*) AS tag_prefix_matches
FROM (SELECT relid_generate('b') AS id FROM generate_series(1, 100)) s WHERE id LIKE relid_tag('b') || '-%';
SELECT relid_timestamp('0AQKFF-81ARYZ6S41-RJ6HB7H6NW');
SELECT relid_generate(NULL) IS NULL AS relid_strict_null;
CREATE ROLE idgenkit_relid_reader;
SET ROLE idgenkit_relid_reader;
DO $$
BEGIN
  PERFORM current_setting('idgenkit.relid_secret');
  RAISE EXCEPTION 'idgenkit.relid_secret is readable';
EXCEPTION WHEN insufficient_privilege THEN
  RAISE NOTICE 'secret hidden from non-superusers';
END $$;
SELECT relid_tag('customer-42') AS reader_tag;
RESET ROLE;
DROP ROLE idgenkit_relid_reader;
RESET idgenkit.relid_secret;
SELECT relid_generate('k');

-- Snowflake
SELECT current_setting('idgenkit.machine_id') AS machine_id,
       current_setting('idgenkit.snowflake_epoch_ms') AS epoch_ms;
SELECT count(DISTINCT snowflake_generate()) AS distinct_snowflakes FROM generate_series(1, 20000);
WITH g AS (SELECT n, snowflake_generate() AS id FROM generate_series(1, 20000) n)
SELECT bool_and(id > prev) AS increasing
FROM (SELECT id, lag(id) OVER (ORDER BY n) AS prev FROM g) s WHERE prev IS NOT NULL;
SELECT snowflake_machine_id(snowflake_generate()) AS parsed_machine_id;
SELECT abs(extract(epoch FROM snowflake_timestamp(snowflake_generate()) - now())) < 5 AS sf_time_is_now;
-- vector from testdata/snowflake.txt (epoch 1288834974657, machine 42, sequence 7)
SELECT snowflake_timestamp(1934266310456418311) AS ts,
       snowflake_machine_id(1934266310456418311) AS mid,
       snowflake_sequence(1934266310456418311) AS seq;
SELECT snowflake_from_timestamp('2025-06-15 15:06:40+00') = 1934266310456418311 - (42 << 12) - 7 AS from_ts;
SELECT snowflake_from_timestamp(now() - interval '1 minute') < snowflake_generate() AS range_scan_bound;
SELECT snowflake_from_timestamp('2000-01-01+00');

-- Nano ID
SELECT nanoid_generate() ~ '^[A-Za-z0-9_-]{21}$' AS nanoid_default;
SELECT length(nanoid_generate(64)) AS len64;
SELECT nanoid_generate(12, '0123456789abcdef') ~ '^[0-9a-f]{12}$' AS nanoid_hex;
SELECT nanoid_generate(alphabet => 'ab', size => 30) ~ '^[ab]{30}$' AS nanoid_named_args;
SELECT count(DISTINCT nanoid_generate()) AS distinct_nanoids FROM generate_series(1, 10000);
SELECT nanoid_generate(0);
SELECT nanoid_generate(1025);
SELECT nanoid_generate(5, '');
SELECT nanoid_generate(5, 'aé');
