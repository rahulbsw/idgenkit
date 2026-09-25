-- Register the idgenkit loadable functions. Copy idgenkit_udf.so into the
-- server's plugin_dir first (SELECT @@plugin_dir;).
CREATE FUNCTION ulid_generate RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION ulid_generate_monotonic RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION ulid_timestamp RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION ulid_to_bin RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION bin_to_ulid RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION uuidv4_generate RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION uuidv7_generate RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION uuidv7_generate_monotonic RETURNS STRING SONAME 'idgenkit_udf.so';
CREATE FUNCTION uuidv7_timestamp RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION snowflake_generate RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION snowflake_timestamp RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION snowflake_machine_id RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION snowflake_sequence RETURNS INTEGER SONAME 'idgenkit_udf.so';
CREATE FUNCTION nanoid_generate RETURNS STRING SONAME 'idgenkit_udf.so';
