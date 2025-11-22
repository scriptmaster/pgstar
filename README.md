# PostgreSQL C Extensions

This repository hosts two PostgreSQL extensions written in C:

- `hello_world` – returns a static greeting string.
- `pgstar` – runs a background worker that listens on a TCP port, sniffs the first bytes of each client request, and dispatches to either a toy HTTP handler or an “other protocol” stub.

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
   make extension-build
   ```

2. **Install into PostgreSQL’s extension directories**

   ```bash
   make extension-install
   ```

   Override the set of extensions with `EXT_DIRS="pgstar"` (or legacy `EXT_DIR=pgstar`) if you want to build/install a subset. The same variable works with every target.

## Using the Extensions

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
   - `pgstar version v1.0.1`

### Enabling the `pgstar` background worker

`pgstar` must be loaded via `shared_preload_libraries`, otherwise the worker will not start. Typical steps:

1. Install the extension’s shared library (`make extension-install`).
2. Update `postgresql.conf`:

   ```
   shared_preload_libraries = 'pgstar'
   ```

3. (Optional) Define an environment file (see below) before starting PostgreSQL.
4. Restart PostgreSQL so the worker can register during shared_preload.

At runtime the worker listens on the configured TCP port, accepts connections, waits for the first bytes sent by the client, and then calls:

- `http/request.c` for HTTP-like payloads (identified by typical HTTP verbs).
- `tcp/protocols/other.c` for everything else.

Both handlers currently just log activity and send back small demo responses, but they establish the pattern for richer protocol implementations. On startup the worker also ensures the schema `pgstar` exists by executing `CREATE SCHEMA IF NOT EXISTS pgstar` via SPI, so any future catalog objects can live under that namespace automatically.

### Configuring `pgstar` ports via `.env`

- During `_PG_init` the worker searches for a `.env` file (override the path with `PGSTAR_ENV_PATH`). It loads the first `PGSTAR_PORT=<value>` (or generic `PORT=<value>`) entry.
- If the file cannot be read, is malformed, or the value is missing/invalid, the worker falls back to port `5480`.
- Example `.env` contents:

   ```
   PGSTAR_PORT=7777
   ```

Create or edit the file from the repo root (e.g. `touch /root/workspace/pg/.env`) and restart PostgreSQL so the background worker picks up the new configuration.

## Rebuilding / Cleaning

- Clean artifacts: `make extension-clean`
- Rebuild everything: `make extension-build`
- Remove installed shared libraries + SQL files: `make extension-uninstall`
