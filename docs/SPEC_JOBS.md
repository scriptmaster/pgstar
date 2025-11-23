## pgstar Jobs (`pgstar.jobs` / `pgstar.job_runs`)

The jobs subsystem lets `pgstar` track and execute batched or continuous work
over ranges of table keys. It is backed by the `pgstar.jobs` and
`pgstar.job_runs` tables plus C helpers in `functions/pgstar_jobs.c`.

### Table: `pgstar.jobs`

Created at worker startup in `core/worker.c`:

- **job_id**: `bigserial primary key`
- **job_type**: `text not null check (job_type in ('run_once','cron','continuous'))`
- **cron_expression**: `text`
- **table_name**: `text not null`
- **key_column**: `text not null`
- **given_stmt**: `text not null`
- **parameters**: `jsonb not null default '{}'::jsonb`
- **range_direction**:
  - `text not null default 'asc'`
  - `check (range_direction in ('asc','desc'))`
- **start_range**: `numeric not null`
- **end_range**: `numeric`
- **last_record**: `numeric`
- **batch_count**: `integer not null default 1000`
- **start_time**: `timestamptz not null default now()`
- **duration**: `interval`
- **last_updated_time**: `timestamptz`
- **completed_time**: `timestamptz`
- **records_processed**: `bigint not null default 0`

Jobs describe *what* to run (`given_stmt`, `parameters`) and *how* to step
through key ranges (`start_range`, `end_range`, `range_direction`,
`batch_count`). They also provide basic *progress* tracking via
`last_record`, `records_processed`, and timestamps, which you can use to
resume or extend a job window (for example, syncing the next 3‑month slice
of telemetry data) using the same metadata. The current implementation
focuses on metadata and basic creation/introspection; scheduling/execution
orchestration is intentionally minimal and can be extended.

### Table: `pgstar.job_runs`

Also created at startup:

- **job_run_id**: `bigserial primary key`
- **job_id**: `bigint not null references pgstar.jobs(job_id) on delete cascade`
- **table_name**: `text`
- **key_column**: `text`
- **start_time**: `timestamptz not null default now()`
- **start_range_number**: `numeric`
- **start_batch**: `integer`
- **previous_completion**: `timestamptz`
- **records_retrieved**: `bigint`
- **records_processed**: `bigint`
- **processed_time**: `interval`
- **executed_stmt**: `text`
- **parameters**: `jsonb not null default '{}'::jsonb`

Each row records a concrete execution slice of a job (e.g. one batch of key
values), enabling progress reporting and history.

### Function: `pgstart_create_job(...) -> bigint`

Defined in `functions/pgstar_jobs.c`, exposed via SQL in
`pgstar--*.sql` as:

```sql
CREATE OR REPLACE FUNCTION pgstart_create_job(
  job_type        text,
  cron_expression text,
  table_name      text,
  key_column      text,
  given_stmt      text,
  parameters      jsonb,
  range_direction text,
  start_range     numeric,
  end_range       numeric DEFAULT NULL,
  batch_count     integer DEFAULT 1000
) RETURNS bigint
```

- **Validations**:
  - `job_type` must be one of `run_once`, `cron`, `continuous`.
  - `range_direction` must be `asc` or `desc`.
  - `batch_count` ≤ 0 is coerced to `1000`.
- **Behaviour**:
  - Inserts a new row into `pgstar.jobs`.
  - Returns the generated `job_id`.
- **Usage**:
  - Intended to be called from SQL or HTTP `/api/exec` to register work.

Example: sync telemetry locations from Azure Postgres into `locations2025`

1. **Local target table with rounded coordinates and de‑duplication**

   ```sql
   CREATE TABLE IF NOT EXISTS locations2025 (
     id SERIAL PRIMARY KEY,
     long      NUMERIC(9,4) NOT NULL,
     lat       NUMERIC(9,4) NOT NULL,
     long_raw  DOUBLE PRECISION,
     lat_raw   DOUBLE PRECISION,
     short_address VARCHAR(255),
     long_address  VARCHAR(1024),
     precision INTEGER DEFAULT 4 CHECK (precision BETWEEN 0 AND 15),
     source    VARCHAR(16) CHECK (source IN ('history','api','postgis','nom','python'))
   );

   CREATE INDEX IF NOT EXISTS idx_locations2025_long
     ON locations2025 (long);

   CREATE INDEX IF NOT EXISTS idx_locations2025_lat
     ON locations2025 (lat);

   -- Prevent duplicate rows for the same rounded location + addresses.
   CREATE UNIQUE INDEX IF NOT EXISTS ux_locations2025_unique_loc_addr
     ON locations2025 (long, lat, short_address, long_address);
   ```

2. **FDW to the remote `TelemetryHistory` table in Azure**

   ```sql
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
   ```

3. **Register a `run_once` job that syncs from “yesterday” back to 3 months ago**

   - The job:
     - Uses `recorded_at` on the FDW table as its logical key.
     - Encodes time as `EXTRACT(EPOCH FROM recorded_at)::numeric` in
       `start_range` / `end_range`.
     - Rounds the remote `POINT` coordinates to `NUMERIC(9,4)` and inserts
       into `locations2025`.
     - Processes **1000 rows per batch**, descending in time, and avoids
       duplicates via the unique index.

   ```sql
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
     1000                                -- batch_count
   );
   ```

   - **Restarting / extending the sync**:
     - The numeric range encodes a time window; you can register a new job
       with a different `[start_range, end_range]` (for example, “next
       3 months” or “older than 6 months”) while still using the same
       `given_stmt`.
     - An execution engine built on top of `pgstar` can update
       `pgstar.jobs.last_record` and `records_processed` after each batch,
       allowing you to resume from where you left off or schedule periodic
       backfills (for example, once every 24 hours).

### Function: `pgstar_job_status(job_id bigint) -> jsonb`

Returns a JSONB view combining a job and its runs:

- **Shape**:

```json
{
  "job": { ... job columns ... },
  "runs": [
    { ... job_runs row 1 ... },
    { ... job_runs row 2 ... }
  ]
}
```

- **Semantics**:
  - Aggregates all `pgstar.job_runs` rows for the given `job_id`.
  - Returns `NULL` if the job does not exist or no data is available.

This is useful for dashboards built on top of `pgstar` (for example via the UI
module documented in `SPEC_UI.md`) to surface progress and history for jobs.
 
### Worker initialization flow for jobs tables

The `pgstar` background worker ensures that the jobs metadata tables exist at startup.
The high‑level flow from the worker entrypoint until it looks for (and creates if
needed) the jobs tables is:

```mermaid
flowchart TD
  A[Background worker entrypoint<br/>pgstar_worker (core/worker.c)] --> B[Connect to database<br/>BackgroundWorkerInitializeConnection]
  B --> C[Ensure pgstar schema<br/>pgstar_execute_sql('CREATE SCHEMA IF NOT EXISTS pgstar')]
  C --> D[Ensure default user<br/>pgstar_ensure_default_user]
  D --> E[Ensure jobs tables<br/>pgstar_ensure_jobs_table]
  E --> F[CREATE TABLE IF NOT EXISTS pgstar.jobs]
  E --> G[CREATE TABLE IF NOT EXISTS pgstar.job_runs]
```

### Jobs execution engine

There are two intended ways to execute work described by `pgstar.jobs`:

- **External orchestrator (recommended for complex flows)**:
  - **Select** from `pgstar.jobs` to discover pending work.
  - **Execute** the `given_stmt` for appropriate key ranges and batches:
    - Use `start_range`, `end_range`, and `range_direction` to drive your own
      paging logic.
    - It is common to encode the range window as parameters `$1` / `$2` in
      `given_stmt` and call it through SQL or `/api/exec`.
  - **Record progress** by:
    - Incrementing `pgstar.jobs.records_processed` and updating
      `last_record`, `last_updated_time`, `completed_time`.
    - Appending rows to `pgstar.job_runs` with the concrete execution slices.

- **Built‑in background worker (simple run‑once executor)**:
  - Implemented in `core/jobs_worker.c` and registered from `pgstar/main.c`
    as the `pgstar jobs executor` background worker.
  - **Scope**:
    - Only handles `job_type = 'run_once'` jobs.
    - Treats each job as a **single execution of `given_stmt`** over the
      configured `[start_range, end_range]` window.
    - Assumes `given_stmt` accepts two numeric parameters `$1` and `$2` for
      the window bounds (they map to `start_range` and `end_range`).
  - **Execution flow per job**:
    1. Select *one* pending `run_once` job:
       - `completed_time IS NULL`.
       - Ordered by `start_time`.
       - Locked with `FOR UPDATE SKIP LOCKED` so future multi‑worker
         designs can coexist.
    2. Execute `given_stmt` via SPI:
       - Called as `given_stmt($1 := start_range, $2 := end_range)` using
         `SPI_execute_with_args(...)`.
       - Uses `batch_count` as the `tcount` hint to SPI, which limits the
         number of processed rows for `SELECT`‑like statements.
    3. Insert a row into `pgstar.job_runs`:
       - Copies `job_id`, `table_name`, `key_column`, `start_range` and
         `parameters`.
       - Sets `records_retrieved` / `records_processed` to `SPI_processed`.
       - Stores the executed SQL text in `executed_stmt`.
       - Records the previous `completed_time` (if any) in
         `previous_completion`.
    4. Update the parent `pgstar.jobs` row:
       - `records_processed` is incremented by `SPI_processed`.
       - `last_updated_time` is set to `now()`.
       - `completed_time` is set to `now()` (the built‑in worker treats
         `run_once` jobs as fully completed after a single execution).
  - **Limitations / design notes**:
    - The built‑in worker **does not** currently:
      - Update `last_record`.
      - Split work into multiple `job_runs` batches for a single job.
      - Interpret `cron` or `continuous` jobs.
    - For advanced scheduling, true batching, and resumable windows, use an
      external orchestrator that builds on the same metadata and helper
      functions.
