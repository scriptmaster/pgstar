## pgstar FDW / Data Access Notes

`pgstar` does not currently implement a Foreign Data Wrapper (FDW) in the sense
of PostgreSQL's `CREATE FOREIGN DATA WRAPPER` API. Instead, it focuses on:

- Embedding an HTTP server in a background worker.
- Exposing JSON APIs over HTTP.
- Executing SQL and procedures via SPI.

This document outlines how FDW-like patterns can be achieved today and where a
future FDW integration could fit.

### Current capabilities

- **Read access**:
  - `/api/:schema/:table` exposes tables as JSON via HTTP.
  - `/api/exec` can run arbitrary SQL or stored procedures.
  - `pgstar_json_schemata()` and `pgstar_json_tables()` provide catalog
    metadata as JSON.
- **Write / procedure execution**:
  - `/api/exec?sql=...` or `/api/exec?sp=...` can invoke write statements or
    server-side procedures.

In many architectures this acts as an application-facing "data API" layer,
similar to some REST/GraphQL data services, but fully inside PostgreSQL.

### Shaping a future FDW

A true FDW integration could:

- Implement `Handler` and `Validator` functions to expose remote HTTP/JSON
  resources as foreign tables.
- Use the existing HTTP client helpers:
  - `pgstar_http_get(url text, headers jsonb) -> text`
  - `pgstar_http_post(url text, headers jsonb, body text) -> text`
- Translate foreign table scans into HTTP GET/POST calls.
- Reuse `pgstar`'s JSON parsing utilities and error handling patterns.

This would live alongside the current HTTP server functionality and would be
documented in this file once implemented. For now, `SPEC_FDW.md` records the
design space and constraints rather than a concrete FDW API.




