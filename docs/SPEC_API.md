## pgstar HTTP API (`/api/*`)

The `pgstar` background worker exposes a small JSON HTTP API that runs inside the
database using SPI.

### Routing overview

All HTTP requests are parsed by `http/request.c` and dispatched through
`http/router.c`:

- `/api/*` → `pgstar_route_api` in `http/api.c`
- `/pgstar/*` → `pgstar_route_pgstar` in `http/route_pgstar.c`
- `/ai/*` → `pgstar_route_ai` in `http/ai.c`
- `/static/*` → static / UI handlers

### General behaviour

- Responses are JSON with `application/json` content type, unless otherwise noted.
- Errors are returned as JSON structures, never bare text.
- All SPI calls are wrapped in proper transactions:
  - `StartTransactionCommand()`
  - `PushActiveSnapshot()`
  - `SPI_connect()` / `SPI_finish()`
  - `CommitTransactionCommand()`

### Endpoint: `GET /pgstar/version`

- **Handler**: `pgstar_route_pgstar`
- **Response**:
  - 200 OK:
    - `{"service":"pgstar","version":"<PGSTAR_VERSION>"}`
  - 404 for any other `/pgstar/*` paths.

### Endpoint: `GET /api/information_schema/schemata`

- **Handler**: `pgstar_route_api` → `pgstar_api_information_schema`
- **Description**:
  - Returns a JSON representation of available schemata.
- **Response**:
  - 200 OK with JSON from `pgstar_json_schemata()`.
  - 500 with `{"error":"schemata query failed"}` on error.

### Endpoint: `GET /api/information_schema/tables`

- **Handler**: `pgstar_route_api` → `pgstar_api_information_schema`
- **Description**:
  - Returns a JSON representation of base tables.
- **Response**:
  - 200 OK with JSON from `pgstar_json_tables()`.
  - 500 with `{"error":"tables query failed"}` on error.

### Endpoint: `GET|POST /api/exec`

- **Handler**: `pgstar_route_api` → `pgstar_api_exec`
- **Query parameters**:
  - `sql`: raw SQL to execute.
  - `sp`: stored procedure name; when provided, runs `SELECT * FROM sp()`.
- **Constraints**:
  - Only `GET` and `POST` allowed; other methods return:
    - 405 `{"error":"method not allowed"}`
- **Behaviour**:
  - Exactly one of `sql` or `sp` must be non-empty.
  - On success:
    - Returns a JSON object:
      - `errors`: an array of error messages (typically empty on success).
      - `rows_affected`: SPI row count.
      - `result`: JSON array of row objects with `"column":"value"` pairs.
  - On error:
    - Returns 500 with `errors` containing the error message.

Example success payload:

```json
{
  "errors": [],
  "rows_affected": 1,
  "result": [
    { "col1": "value1", "col2": "value2" }
  ]
}
```

### Endpoint: `GET /api/:schema/:table`

- **Handler**: `pgstar_route_api` → `pgstar_api_generic_table`
- **Example**:
  - `/api/public/orders`
- **Behaviour**:
  - Disallows `information_schema` schema via this generic route.
  - Uses a dynamically built `SELECT` list from `?select=` query parameter:
    - Only allows safe characters (`[a-zA-Z0-9_, ]`) to avoid SQL injection.
    - When missing, defaults to `*`.
  - Executes:
    - `SELECT <select_list> FROM "<schema>"."<table>"`
  - Streams rows into a JSON array of objects.
- **Responses**:
  - 200 with JSON array of rows on success.
  - 404 with `{"error":"invalid api path"}` for malformed paths.
  - 500 with `{"error":"generic api query failed"}` on SPI errors.




