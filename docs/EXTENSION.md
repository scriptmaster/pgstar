# pgstar Extension Installation Guide

## Overview

The `pgstar` extension installation is a **two-step process**:

1. **Build and install the shared library** (binary files) - done once per PostgreSQL server
2. **Create the extension in a database** - done per database where you want to use pgstar

## Step 1: Build and Install Shared Library

This step compiles the C code and installs the shared library (`pgstar.so`) and SQL files into PostgreSQL's extension directories. This is done **once per PostgreSQL server installation**.

### Build Process

```bash
# Build the extension (compiles C code)
make build
# or
make extension-build-pgstar
```

This compiles all C source files in:
- `pgstar/*.c`
- `pgstar/core/*.c`
- `pgstar/api/*.c`
- `pgstar/tcp/*.c`
- `pgstar/tcp/protocols/*.c`
- `pgstar/http/*.c`
- `pgstar/functions/*.c`
- `pgstar/ui/*.c`

### Install Process

```bash
# Install shared library and SQL files to PostgreSQL directories
make install
# or
make extension-install-pgstar
```

This uses PostgreSQL's PGXS (PostgreSQL Extension System) to install:
- **Shared library**: `pgstar.so` → `$libdir/pgstar.so` (typically `/usr/lib/postgresql/14/lib/pgstar.so`)
- **SQL files**: `pgstar--1.21.sql` → `share/extension/pgstar--1.21.sql`
- **Control file**: `pgstar.control` → `share/extension/pgstar.control`

**Note**: This step requires superuser privileges and does NOT create anything in any database yet.

## Step 2: Create Extension in Database

This step creates the extension objects (functions, tables, schemas) **in a specific database**. The extension is created in **whichever database you connect to** when running `CREATE EXTENSION`.

### Installation Location

**The extension is installed in the database where you run `CREATE EXTENSION`.**

For example:

```sql
-- Connect to 'postgres' database
\c postgres
CREATE EXTENSION pgstar;
-- Extension is now installed in 'postgres' database

-- Connect to 'mydb' database  
\c mydb
CREATE EXTENSION pgstar;
-- Extension is now installed in 'mydb' database (separate instance)
```

### What Happens During CREATE EXTENSION

When you run `CREATE EXTENSION pgstar`, PostgreSQL:

1. **Reads the control file** (`pgstar.control`) to determine the version
2. **Executes the SQL file** (`pgstar--1.21.sql`) which:
   - Creates functions (`pgstar_version()`, `pgstar_http_post()`, etc.)
   - Creates the `pgstar` schema
   - Creates the `pgstar.pages` table
   - Inserts seed data into `pgstar.pages`
   - Creates other tables (users, jobs, etc. via background worker)
3. **Registers the extension** in `pg_extension` catalog table

### Example Installation

```sql
-- Connect to your target database
\c postgres

-- Drop existing extension if needed (CASCADE removes all objects)
DROP EXTENSION IF EXISTS pgstar CASCADE;

-- Create the extension
CREATE EXTENSION pgstar;

-- Verify installation
SELECT pgstar_version();
SELECT * FROM pgstar.pages;
```

### Files Created in Database

After `CREATE EXTENSION pgstar`, the following are created **in the database**:

- **Schema**: `pgstar` schema
- **Tables**: 
  - `pgstar.pages` (with seed data)
  - `pgstar.jobs` (created by background worker)
  - `pgstar.job_runs` (created by background worker)
  - `pgstar.users` (created by background worker)
- **Functions**: 
  - `pgstar_version()`
  - `pgstar_http_post()`
  - `pgstar_http_get()`
  - `pgstart_create_job()`
  - `pgstar_job_status()`

## Background Worker vs Extension Installation

**Critical Distinction**: Background workers and extension installation are **completely separate**:

### Background Worker (Server-Level)

- **Registered ONCE** when PostgreSQL starts (via `shared_preload_libraries`)
- **Starts ONCE** per PostgreSQL server instance
- **Connects to ONE database** specified by `pgstar.database` GUC
- **Listens on ONE port** (default: 5481)
- **Independent** of how many databases have the extension installed

The worker registration happens in `_PG_init()` which is called when PostgreSQL loads the shared library, **not** when you run `CREATE EXTENSION`.

### Extension Installation (Database-Level)

- **Created per database** where you run `CREATE EXTENSION`
- Each database gets its own copy of:
  - Functions (`pgstar_version()`, etc.)
  - Tables (`pgstar.pages`, `pgstar.jobs`, etc.)
  - Schema (`pgstar`)

### Example Scenario

If you install the extension in two databases:

```sql
-- Database 1
\c mydb1
CREATE EXTENSION pgstar;
-- Creates: mydb1.pgstar schema, mydb1.pgstar.pages table, etc.

-- Database 2  
\c mydb2
CREATE EXTENSION pgstar;
-- Creates: mydb2.pgstar schema, mydb2.pgstar.pages table, etc.
```

**Result**:
- ✅ **Two separate sets** of extension objects (one in each database)
- ✅ **Only ONE background worker** running (registered at server startup)
- ✅ **Worker connects to ONE database** (specified by `pgstar.database`)

The worker will only be able to access tables/functions in the database it connects to. If the worker connects to `mydb1`, it can only query `mydb1.pgstar.pages`, not `mydb2.pgstar.pages`.

### Background Worker Configuration

The background worker connects to a database specified by the `pgstar.database` GUC setting (defaults to `"postgres"`). This is configured in `postgresql.conf`:

```conf
pgstar.database = 'postgres'  # Default: 'postgres'
pgstar.user = 'postgres'       # Default: NULL (uses current user)
```

**Important**: The background worker connects to the database specified by `pgstar.database`, **not** necessarily the database where the extension was created. However, since the extension objects (tables, functions) are created in the database where `CREATE EXTENSION` was run, you should:

1. Create the extension in the same database that `pgstar.database` points to, OR
2. Ensure the background worker connects to the database where you created the extension

### Recommended Setup

For simplicity, use the `postgres` database (the default):

1. **Create extension in `postgres` database**:
   ```sql
   \c postgres
   CREATE EXTENSION pgstar;
   ```

2. **Configure worker to use `postgres` database** (default):
   ```conf
   # In postgresql.conf (or leave default)
   pgstar.database = 'postgres'
   ```

This ensures the background worker and extension objects are in the same database.

## Extension Versioning

The extension version is determined by `default_version` in `pgstar.control`:

```
default_version = '1.21'
```

The corresponding SQL file must be named `pgstar--1.21.sql`. When you upgrade the version:

1. Update `default_version` in `pgstar.control`
2. Create a new SQL file `pgstar--1.22.sql` (or next version)
3. Optionally create an upgrade script `pgstar--1.21--1.22.sql` for migrations

## Uninstallation

To remove the extension from a database:

```sql
-- Remove extension and all its objects
DROP EXTENSION pgstar CASCADE;
```

This removes:
- All functions created by the extension
- The `pgstar` schema and all tables
- All data in those tables

**Note**: The shared library (`pgstar.so`) remains installed on the filesystem. To remove it:

```bash
make extension-uninstall-pgstar
```

## Troubleshooting

### Extension not found

If you get `ERROR: extension "pgstar" does not exist`:

1. Verify the shared library is installed:
   ```bash
   ls -la $(pg_config --pkglibdir)/pgstar.so
   ```

2. Verify the SQL file exists:
   ```bash
   ls -la $(pg_config --sharedir)/extension/pgstar--*.sql
   ```

3. Ensure PostgreSQL can find the extension:
   ```sql
   SELECT * FROM pg_available_extensions WHERE name = 'pgstar';
   ```

### Tables not found

If tables like `pgstar.pages` don't exist:

1. Verify the extension was created:
   ```sql
   SELECT * FROM pg_extension WHERE extname = 'pgstar';
   ```

2. Check if the SQL file executed successfully (check PostgreSQL logs)

3. Verify you're connected to the correct database:
   ```sql
   SELECT current_database();
   ```

### Background worker can't find tables

If the background worker can't find tables:

1. Verify `pgstar.database` points to the database where extension was created:
   ```sql
   SHOW pgstar.database;
   ```

2. Check background worker logs for connection errors

3. Ensure the extension was created in the database specified by `pgstar.database`

## Summary

- **Shared library installation** (`make install`): Installs binaries to PostgreSQL directories (once per server)
- **Background worker registration**: Happens when PostgreSQL starts (via `shared_preload_libraries`), **not** when you run `CREATE EXTENSION`
- **Background worker**: Only **ONE** worker runs per PostgreSQL server, connects to **ONE** database (specified by `pgstar.database` GUC)
- **Extension creation** (`CREATE EXTENSION`): Creates objects in a specific database (per database)
- **Multiple databases**: You can install the extension in multiple databases, but only **ONE** background worker will run
- **Default database**: Extension is typically created in `postgres` database
- **Best practice**: Create extension and configure worker to use the same database (`postgres`)

### Key Takeaway

**Installing the extension in multiple databases does NOT start multiple background workers.** The worker is registered once at server startup and connects to one database. If you need workers for multiple databases, you would need to modify the code to register multiple workers or use a different architecture.

