Project Context & Architecture
Overview

pgstar is a PostgreSQL extension that transforms Postgres into a self-hosted Web Fullstack framework and Data Science system.

Core: C extension using Background Workers (pgstar_worker).

Networking: Custom TCP/HTTP stack (tcp/*.c, http/*.c) with OpenSSL support (-lssl -lcrypto).

Frontend: Vue.js (migrated from Datastar).

Storage:

pgstar.files: Static assets (JS/CSS/Vue). Every payload lives in text_data:

Plain UTF-8 for textual assets

Base64-encoded text for binary assets (favicon, images, etc.)

Remote CDN/HTTP sources recorded via download_url when is_download=TRUE

pgstar.pages: Legacy/Dynamic HTML pages.

Critical Coding Standards
1. Memory Management (CRITICAL)

PostgreSQL uses a hierarchical memory context system.

SPI Contexts: Data returned by SPI_execute (via SPI_getvalue, SPI_getbinval, etc.) lives in the transaction context.

Rule: NEVER return pointers from SPI directly. ALWAYS deep copy into the caller's memory context.

SPI_getvalue() and pstrdup() Rules

SPI_getvalue() returns pointers only valid until SPI_finish() is called.

They must be copied immediately via pstrdup() (wrapped in PG_TRY/PG_CATCH).

Do NOT use them in:

appendStringInfo()

escape_json()

String operations

Use SPI_getbinval() + OidOutputFunctionCall() instead.

Free all copies with pgstar_safe_pfree().

Varlena / bytea handling

Use DatumGetByteaP() or PG_DETOAST_DATUM_COPY().

Never cast Datum to raw char *.

Returning buffers

When returning buffers (HTTP JSON/text), you must:

Switch to caller’s context

Copy the data

Only then finish the transaction.

2. Binary Safety

Use DatumGetByteaP() for bytea.

Explicitly track body_len—binary can include zeros.

UI assets must be base64 inside TEXT.
No use of bytea in pgstar.files.

3. Worker Hygiene

Long loops must have CHECK_FOR_INTERRUPTS().

HTTP keep-alive → insert interrupt checks to avoid freezing.

4. SQL Querying (SPI)

Prefer quote_literal_cstr() + inlined SQL for simple lookups.

Avoid $1 unless necessary (plan caching issues).

Feature Implementation Details
Assets (pgstar.files)

Single text_data column holds:

UTF-8 for JS/CSS/HTML

Base64 for binary

Lazy downloads:

If is_download = TRUE and download_url IS NOT NULL,
→ worker fetches once, stores in text_data, and clears the flag.

HTTP Client

Supports:

HTTP/1.1

HTTPS through OpenSSL

Chunked responses

Used internally to fetch remote assets.

Frontend Guidelines
UI Component Restrictions

Prohibited components:
title-bar, window-body, window
→ They break layout in modern Vue UI.

Recent Issues & Fixes (Historical Log)
1. JSON API returning integers

Cause: returning buf.data from SPI memory context
Fix: copy to caller context before SPI_finish.

2. Segfaults in HTTP JSON responses

Cause: SPI_getvalue() pointer used in appendStringInfo
Fix: strict pstrdup + PG_TRY/PG_CATCH rules.

3. Crash during downloads

Cause: CommitTransactionCommand() inside PG_CATCH
Fix: must always use AbortCurrentTransaction() in catch blocks.

4. Broken binary data in pgstar.files

Cause: TOASTed pointers reused
Fix: PG_DETOAST_DATUM_COPY.

5. Lazy download crashes

Cause: pointer returned from SPI reused after next SPI call
Fix: own the varlena first (PG_DETOAST_DATUM_COPY).

6. bytea storage removed

All assets now TEXT (base64 or UTF-8)

Eliminates TOAST corruption issues permanently.

Architecture Summary

Server: Runs inside PostgreSQL as a background worker

API: Custom router in C (http/router.c)

UI: Vue.js bundled into pgstar.files

Static files: Stored in TEXT & served directly via Postgres

Pages: Rendered HTML from pgstar.pages

Jobs framework: pgstar.jobs + pgstar.job_runs
