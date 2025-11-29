# Project Context & Architecture

## Overview
`pgstar` is a PostgreSQL extension that transforms Postgres into a self-hosted Web Server and CRM system.
- **Core**: C extension using Background Workers (`pgstar_worker`).
- **Networking**: Custom TCP/HTTP stack (`tcp/*.c`, `http/*.c`).
- **Frontend**: Vue.js (migrated from Datastar).
- **Storage**:
  - `pgstar.files`: Static assets (JS/CSS/Vue) & binary data.
  - `pgstar.pages`: Legacy/Dynamic HTML pages.

## Critical Coding Standards

### 1. Memory Management (CRITICAL)
PostgreSQL uses a hierarchical memory context system.
- **SPI Contexts**: Data returned by `SPI_execute` (e.g., via `SPI_getvalue`) resides in a short-lived transaction context.
- **The Rule**: NEVER store pointers from SPI directly into long-lived structs or caches.
- **The Protocol**:
  1. Save caller's context: `MemoryContext old = CurrentMemoryContext;`
  2. Start Tx: `StartTransactionCommand();`
  3. Query & Get Data.
  4. **Deep Copy** data out: Use `pstrdup` (text) or `palloc`+`memcpy` (binary) to copy data *while inside the transaction*.
  5. Switch to caller context: `MemoryContextSwitchTo(old);`
  6. Assign copied data to return structs.
  7. Commit: `CommitTransactionCommand();` (This destroys the source data).

### 2. Worker Hygiene
- **Interrupts**: Long-running loops (like HTTP keep-alive in `tcp/protocol.c`) MUST call `CHECK_FOR_INTERRUPTS()` to ensure the worker responds to signals and processes invalidation messages.

### 3. Database Querying (SPI)
- **Inlining vs Parameters**: For simple, critical lookups in background workers, prefer **inlining parameters** (`quote_literal_cstr`) over `SPI_execute_with_args` (`$1`).
  - *Reason*: Avoids potential plan caching/parameter binding staleness issues in long-lived sessions.
  - *Example*: Use `WHERE file_name = 'value'` (inlined) instead of `WHERE file_name = $1`.
- **Index Usage**: Inlined literal queries **DO** use indices standardly.

## Recent Issue: Intermittent 404 (Resolved)
- **Symptoms**: `pgstar_ui_fetch_asset` returned "Not Found" for existing files after cache timeout. Hex dumps confirmed strings were identical.
- **Cause**: SPI parameter binding/plan caching issue in the keep-alive worker, compounded by lack of `CHECK_FOR_INTERRUPTS`.
- **Fix**: Added `CHECK_FOR_INTERRUPTS()` and switched to inlined SQL queries for asset fetching.


