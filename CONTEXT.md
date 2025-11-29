# Project Context & Architecture

## Overview
`pgstar` is a PostgreSQL extension that transforms Postgres into a self-hosted Web Server and CRM system.
- **Core**: C extension using Background Workers (`pgstar_worker`).
- **Networking**: Custom TCP/HTTP stack (`tcp/*.c`, `http/*.c`) with OpenSSL support (`-lssl -lcrypto`).
- **Frontend**: Vue.js (migrated from Datastar).
- **Storage**:
  - `pgstar.files`: Static assets (JS/CSS/Vue), binary data (favicon, images), and lazy-downloadable URLs.
  - `pgstar.pages`: Legacy/Dynamic HTML pages.

## Critical Coding Standards

### 1. Memory Management (CRITICAL)
PostgreSQL uses a hierarchical memory context system.
- **SPI Contexts**: Data returned by `SPI_execute` (e.g., via `SPI_getvalue`, `SPI_getbinval`, or `StringInfo` buffers built inside a Tx) resides in a short-lived transaction context.
- **The Rule**: NEVER return pointers from SPI directly. ALWAYS deep copy them to the caller's memory context *before* the transaction ends.
- **The Protocol**:
  1. Save caller's context: `MemoryContext old = CurrentMemoryContext;`
  2. Start Tx: `StartTransactionCommand();`
  3. Query & Get Data.
  4. **Deep Copy** data out: 
     - Strings: `pstrdup` 
     - Binary: `palloc` + `memcpy`
     - **Do this before** `SPI_finish()` or `CommitTransactionCommand()`.
  5. Commit: `CommitTransactionCommand()` (or `AbortCurrentTransaction()` on error).

### 2. Binary Data Handling (CRITICAL)
- **TOAST**: When retrieving `bytea` columns via SPI:
  - **NEVER** cast `Datum` directly to `bytea*` or `char*`.
  - **ALWAYS** use `DatumGetByteaP(datum)` to ensure the data is detoasted (decompressed/fetched from external storage) and is a flat, valid pointer.
- **Caching**: The in-memory UI cache (`pgstar_ui_cache`) must explicitly track `body_len` to support binary data (which may contain null bytes).

### 3. Worker Hygiene
- **Interrupts**: Long-running loops (like HTTP keep-alive in `tcp/protocol.c`) MUST call `CHECK_FOR_INTERRUPTS()` to ensure the worker responds to signals and processes invalidation messages.

### 4. Database Querying (SPI)
- **Inlining vs Parameters**: For simple, critical lookups in background workers, prefer **inlining parameters** (`quote_literal_cstr`) over `SPI_execute_with_args` (`$1`) to avoid plan caching issues.

## Feature Implementation Details

### Asset Management (`pgstar.files`)
- **Dual Mode**: Stores either `text_data` (for code) or `binary_data` (for images/compiled assets).
- **Lazy Downloads**:
  - If `is_download = TRUE` and `text_data` is a URL (http/https):
  - On first access, the worker downloads the content using the built-in HTTP client (supports OpenSSL).
  - It **updates the row** with the downloaded content (into `binary_data` or `text_data`), sets `is_download = FALSE`, and serves the content.
  - This allows "CDN-like" behavior where assets are fetched once and served from the DB thereafter.

### HTTP Client
- **Capabilities**: Supports HTTP/1.1, HTTPS (via OpenSSL), and basic `Transfer-Encoding: chunked`.
- **Usage**: Used internally for downloading assets defined in `pgstar.files`.

## Recent Issues & Fixes
- **JSON API returning Integers**: Caused by returning `buf.data` (from `StringInfo`) which was allocated in the transaction context. Fixed by `pstrdup`ing the result before `SPI_finish`.
- **Intermittent Crashes on Download**: Caused by `CommitTransactionCommand()` in `PG_CATCH` block (illegal state). Fixed by using `AbortCurrentTransaction()`.
- **Broken Binary Data**: Caused by casting TOASTed `datum` directly. Fixed by using `DatumGetByteaP()`.
