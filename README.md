# pgstar

`pgstar` adds to PostgreSQL into a high-performance HTTP server by running a
non-blocking TCP listener inside a **background worker** - the same postgres infrastructure used for replication background service and auto-vacuum background service.

It’s not “Postgres behind an API” – the API actually **lives inside** Postgres:
the worker uses `WaitLatchOrSocket()` for event-driven I/O and SPI for running
SQL directly from the background process.



# Repo Structure

This repository hosts two PostgreSQL extensions written in C:

- `hello_world` – returns a static greeting string.
- `pgstar` – runs a background worker that listens on a TCP port, checks the first bytes of each client request, and dispatches to either a HTTP handler or an “other protocol” stub.

## Files

- `hello_world/` – PGXS Makefile, control file, SQL install script, and C implementation.
- `pgstar/` – background worker entrypoint plus `core/`, `tcp/`, and `http/` helpers.
- `Makefile` – convenience targets for building/cleaning/installing one or both extensions.
- `README.md` – this document.

## Prerequisites

- PostgreSQL server and headers (ensure `pg_config` is on your `PATH`).
- A PostgreSQL superuser role for installing extensions.

## Build & Install

1. **Compile (all extensions by default)**

   ```bash
   make build
   ```

2. **Install into PostgreSQL’s extension directories**

   ```bash
   make install
   ```

   Override the set of extensions with `EXT_DIRS="pgstar"` (or legacy `EXT_DIR=pgstar`) if you want to build/install a subset. The same variable works with every target.


## ⚙️ Install Configuration

In `postgresql.conf`:

```conf
shared_preload_libraries = 'pgstar'

pgstar.port      = 5481
pgstar.log_level = 'info'

## Using the Extension Functions

1. Connect to your target database with `psql`.
2. Create whichever extension you need (only once per database):

   ```sql
   CREATE EXTENSION hello_world;
   CREATE EXTENSION pgstar;
   ```

3. Call the demo functions:

   ```sql
   SELECT hello_world();
   SELECT pgstar_version();
   ```

   Example outputs:

   - `Hello from a C extension!`
   - `pgstar version v1.0.3`

### Enabling & configuring the `pgstar` background worker

`pgstar` must be loaded via `shared_preload_libraries`, otherwise the worker will not start. Typical steps:

1. Install the shared library (`make extension-install`).
2. Update `postgresql.conf`:

   ```
   shared_preload_libraries = 'pgstar'
   pgstar.port = 5481
   pgstar.log_level = 'info'
   pgstar.database = 'postgres'
   pgstar.user = 'postgres'
   ```

3. Restart PostgreSQL so `_PG_init` can register the worker.

The new GUCs let you control which database/user the worker connects to and how noisy its logs should be. The port must be between 1025 and 65535; the log level can be `error`, `warning`, `info`, `debug1`, or `debug2`.

At runtime the worker listens on the configured TCP port, accepts connections, sniffs the first bytes that each client sends, and dispatches to:

- `http/request.c` + `http/router.c` for HTTP payloads.
- `tcp/protocols/other.c` for everything else.

The HTTP handler now exposes a small JSON API, including:

- `GET /` – health check
- `GET /pgstar/version` – returns `PGSTAR_VERSION`
- `GET /api/information_schema/schemata` – dumps schemata as JSON via SPI
- `GET /api/information_schema/tables` – dumps base tables as JSON

Responses are produced through `http/response.c`, which also exposes helpers for JSON and HTML content, and `pgstar_route_request` logs and routes everything consistently.

On startup the worker ensures the schema `pgstar` exists by running `CREATE SCHEMA IF NOT EXISTS pgstar` through `core/spi.c`, so any catalog objects you create for HTTP handlers have a home automatically.

## Rebuilding / Cleaning

- Clean artifacts: `make extension-clean`
- Rebuild everything: `make extension-build`
- Remove installed shared libraries + SQL files: `make extension-uninstall`
