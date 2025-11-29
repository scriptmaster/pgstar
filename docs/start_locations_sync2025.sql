-- start_locations_sync2025.sql
--
-- One-time setup + job registration to sync telemetry locations from the
-- remote Azure Postgres "TelemetryHistory" table into the local
-- "locations2025" table using the pgstar jobs subsystem.
--
-- Assumptions:
--   - The pgstar extension is installed in the current database.
--   - The "locations2025" table has been created (see locations2025.sql).
--   - The remote table "TelemetryHistory" has columns:
--       location      POINT
--       short_address TEXT
--       long_address  TEXT
--       recorded_at   TIMESTAMPTZ
--
-- This script prepares an FDW and registers a run_once job that:
--   - Walks from "yesterday" back to "3 months ago" (descending by time).
--   - Uses EXTRACT(EPOCH FROM recorded_at)::numeric as the range key.
--   - Processes 1000 rows per batch.
--   - Rounds location POINT to NUMERIC(9,4) long/lat and keeps raw doubles.
--   - Avoids duplicates via a unique index on (long, lat, short_address, long_address).

-- Optional: ensure the target table exists
-- \i locations2025.sql

-- 1. FDW to Azure Postgres TelemetryData / TelemetryHistory

CREATE EXTENSION IF NOT EXISTS postgres_fdw;

CREATE SERVER IF NOT EXISTS sampg_telemetrydata_srv
  FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (
    host    'sampg.postgres.database.azure.com',
    dbname  'TelemetryData',
    port    '5432',
    sslmode 'require'
  );

CREATE USER MAPPING IF NOT EXISTS FOR CURRENT_USER
  SERVER sampg_telemetrydata_srv
  OPTIONS (
    user     'dbadmin',
    password 'MY_PASS_HERE'
  );

CREATE SCHEMA IF NOT EXISTS telemetry_fdw;

CREATE FOREIGN TABLE IF NOT EXISTS telemetry_fdw.telemetry_history (
  telemetry_id  BIGINT,
  location      POINT        NOT NULL,
  short_address TEXT,
  long_address  TEXT,
  recorded_at   TIMESTAMPTZ  NOT NULL
)
SERVER sampg_telemetrydata_srv
OPTIONS (schema_name 'public', table_name 'TelemetryHistory');

-- 2. Register a pgstar job to sync locations into locations2025
--
-- The job uses EXTRACT(EPOCH FROM recorded_at)::numeric as its numeric key
-- so that start_range/end_range describe a time window. With the current
-- implementation, pgstar records job metadata; an external executor can
-- read this metadata and drive batched execution, updating last_record and
-- records_processed as it goes.

SELECT pgstart_create_job(
  'run_once',                         -- job_type
  NULL,                               -- cron_expression (not used for run_once)
  'telemetry_fdw.telemetry_history',  -- table_name (driving table)
  'recorded_at',                      -- key_column (time-based key)
  $$
  INSERT INTO locations2025 (
    long,
    lat,
    long_raw,
    lat_raw,
    short_address,
    long_address,
    precision,
    source
  )
  SELECT
    round(CAST(th.location[0] AS numeric), 4)::numeric(9,4) AS long,
    round(CAST(th.location[1] AS numeric), 4)::numeric(9,4) AS lat,
    th.location[0] AS long_raw,
    th.location[1] AS lat_raw,
    th.short_address,
    th.long_address,
    4          AS precision,
    'history'  AS source
  FROM telemetry_fdw.telemetry_history th
  WHERE EXTRACT(EPOCH FROM th.recorded_at)::numeric
        BETWEEN LEAST($1, $2) AND GREATEST($1, $2)
  ORDER BY th.recorded_at DESC
  LIMIT 1000
  ON CONFLICT (long, lat, short_address, long_address) DO NOTHING
  $$,
  '{}'::jsonb,                        -- parameters (unused here)
  'desc',                             -- range_direction: walk backwards in time
  EXTRACT(EPOCH FROM now() - interval '1 day')::numeric,     -- start_range
  EXTRACT(EPOCH FROM now() - interval '3 months')::numeric,  -- end_range
  1000                                -- batch_count (matches LIMIT 1000)
) AS job_id;

-- After running this script (for example via "make run-locations-sync2025"),
-- you can inspect the registered job with:
--
--   SELECT pgstar_job_status(<job_id_from_above>);
--
-- To extend to another window (for example, older than 6 months or the next
-- 3 months), register another job with the same given_stmt but different
-- start_range / end_range based on EXTRACT(EPOCH FROM recorded_at).


