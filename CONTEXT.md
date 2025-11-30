# Project Context & Architecture

## Overview
`pgstar` is a PostgreSQL extension that transforms Postgres into a self-hosted Web Server and CRM system.
- **Core**: C extension using Background Workers (`pgstar_worker`).
- **Networking**: Custom TCP/HTTP stack (`tcp/*.c`, `http/*.c`) with OpenSSL support (`-lssl -lcrypto`).
- **Frontend**: Vue.js (migrated from Datastar).
- **Storage**:
  - `pgstar.files`: Static assets (JS/CSS/Vue). Every payload lives in `text_data`:
    - Plain UTF-8 for textual assets
    - Base64-encoded text for binary assets (favicon, images, etc.)
    - Remote CDN/HTTP sources recorded via `download_url` when `is_download=TRUE`
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
     - Varlen Datums (text/varchar/bytea): use `PG_DETOAST_DATUM_COPY(datum)` so SPI can safely continue without invalidating your pointer.
     - **Do this before** `SPI_finish()` or `CommitTransactionCommand()`.
     - When returning `StringInfo` buffers to higher layers (HTTP/JSON helpers), explicitly `MemoryContextSwitchTo(old)` before copying so the final response lives in the caller's context.
  5. Commit: `CommitTransactionCommand()` (or `AbortCurrentTransaction()` on error).

### 2. Binary Data Handling (CRITICAL)
- **TOAST**: When retrieving `bytea` columns via SPI:
  - **NEVER** cast `Datum` directly to `bytea*` or `char*`.
  - **ALWAYS** use `PG_DETOAST_DATUM_COPY()` (or at minimum `DatumGetByteaP()`) before the tuple is reused. Copying gives you your own buffer and prevents later SPI calls from clobbering it.
- **Caching**: The in-memory UI cache (`pgstar_ui_cache`) must explicitly track `body_len` to support binary data (which may contain null bytes).

### 3. Worker Hygiene
- **Interrupts**: Long-running loops (like HTTP keep-alive in `tcp/protocol.c`) MUST call `CHECK_FOR_INTERRUPTS()` to ensure the worker responds to signals and processes invalidation messages.

### 4. Database Querying (SPI)
- **Inlining vs Parameters**: For simple, critical lookups in background workers, prefer **inlining parameters** (`quote_literal_cstr`) over `SPI_execute_with_args` (`$1`) to avoid plan caching issues.

## Feature Implementation Details

### Asset Management (`pgstar.files`)
- **Single Storage Column**: `text_data` holds the asset payload. Binary blobs are base64-encoded before insertion; textual assets remain raw UTF-8 strings.
- **Lazy Downloads**:
  - `download_url` stores the upstream HTTP/HTTPS source when `is_download = TRUE`.
  - On first access (and when downloads are enabled), the worker fetches the content, base64-encodes it if necessary, stores the result in `text_data`, clears `download_url`, flips `is_download` to `FALSE`, and serves the decoded body.
  - This keeps all rows within PostgreSQL's TEXT types while still supporting CDN-style seeding.

### HTTP Client
- **Capabilities**: Supports HTTP/1.1, HTTPS (via OpenSSL), and basic `Transfer-Encoding: chunked`.
- **Usage**: Used internally for downloading assets defined in `pgstar.files`.

## Frontend Guidelines

### UI Component Restrictions
- **Window/Title Decorations**: Do NOT use `title-bar`, `window-body`, or `window` anywhere in the frontend for decoration. These components are not allowed in the UI.

## Recent Issues & Fixes
- **JSON API returning Integers**: Caused by returning `buf.data` (from `StringInfo`) which was allocated in the transaction context. Fixed by `pstrdup`ing the result before `SPI_finish`.
- **HTTP JSON responses crashing worker**: `pgstar_json_files()` returned `buf.data` from the SPI context; after `SPI_finish()` the pointer was freed, leading to segfaults under load. Fix: switch to the caller's memory context and copy the JSON string before finishing SPI.
- **Intermittent Crashes on Download**: Caused by `CommitTransactionCommand()` in `PG_CATCH` block (illegal state). Fixed by using `AbortCurrentTransaction()`.
- **Broken Binary Data**: Caused by casting TOASTed `datum` directly. Fixed by using `DatumGetByteaP()`.
- **Lazy Download Segfault**: Binary pointers pulled from `SPI_getbinval` were reused after more SPI work. Fixed by taking ownership with `PG_DETOAST_DATUM_COPY()` so subsequent queries cannot free the data we are still using.
- **bytea Storage Removed**: Large assets now live as TEXT (plain or base64). Schema introduces `download_url` for lazy fetches, and the UI layer base64 encodes/decodes automatically to avoid TOAST lifetime issues.
