## pgstar UI Module (pages / JS / CSS)

This document describes the UI layer implemented by the `pgstar` extension, backed
by the `pgstar.pages` catalog table and HTTP routes served from the background
worker.

### Table: `pgstar.pages`

- **page_id**: `bigserial primary key`
- **page_name**: `text unique`
  - HTML pages: logical names like `login`, `dashboard`
  - JS assets: `js/<filename.js>` (e.g. `js/pages_index.js`)
  - CSS assets: `css/<filename.css>` (e.g. `css/pages_index.css`)
- **parent_tag**: `text not null default '' check (parent_tag in ('','body','main'))`
  - `''`  – HTML is treated as a full document or generic fragment
  - `body` / `main` – HTML is treated as a fragment inserted inside `<body>` or `<main>`
- **html**: `text`
  - For HTML pages: fragment markup (typically without `<html>/<head>/<body>`)
  - For JS/CSS assets: full file content served as-is
- **content_type**: `text not null default 'text/html; charset=utf-8'`
  - HTML pages: `text/html; charset=utf-8`
  - JS assets: `application/javascript`
  - CSS assets: `text/css; charset=utf-8`
- **meta**: `jsonb not null default '[]'::jsonb`
  - Array of objects describing meta tags, e.g.:
    - `{"http-equiv":"refresh","content":"0; url=/app/" }`
    - `{"name":"description","content":"pgstar dashboard"}`
- **js**: `text`
  - Comma-separated list of logical JS names *without* prefix (e.g. `pages_index.js`)
- **css**: `text`
  - Comma-separated list of logical CSS names *without* prefix (e.g. `pages_index.css`)
- **is_custom**: `boolean not null default false`
  - `false` – managed by the extension; seed values may be updated on restart
  - `true` – user-managed; seed updates will not overwrite this row
- **updated_at**: `timestamptz not null default now()`

### Seeding behaviour

At worker startup, `pgstar_ui_ensure_pages()` runs and:

- Ensures the `pgstar.pages` table exists.
- Seeds or updates built-in rows *when `is_custom = false`*:
  - `js/pages_index.js`
    - JS bundle that injects the Datastar CDN script:
      `https://cdn.jsdelivr.net/gh/starfederation/datastar@1.0.0-RC.6/bundles/datastar.js`
  - `css/pages_index.css`
    - CSS bundle that imports:
      - Fabric / Office UI Core from its CDN
      - `/static/css/emerald-v2.css`
      - Pico CSS `https://cdn.jsdelivr.net/npm/@picocss/pico@2.1.1/css/pico.classless.min.css`
  - `css/emerald-v2.css`
    - Lightweight placeholder emerald theme (safe to override).
  - `login`
    - `parent_tag = 'main'`, `content_type = 'text/html; charset=utf-8'`
    - Simple login form using Datastar attributes.
    - `js = 'pages_index.js'`, `css = 'pages_index.css'`.
  - `dashboard`
    - `parent_tag = 'main'`, `content_type = 'text/html; charset=utf-8'`
    - Simple dashboard welcome page.
    - `js = 'pages_index.js'`, `css = 'pages_index.css'`.

If a row exists with the same `page_name` and `is_custom = false`, it is updated
in-place with the seed definition. If `is_custom = true`, the row is left
unchanged.

### HTTP routes

All routes are served by the background worker via `http/router.c`.

#### Root `/`

- **Handler**: embedded HTML in `pgstar_root_html` (compiled from `ui/root.html`).
- **Behaviour**:
  - Returns a static landing page.
  - You can edit `pgstar/ui/root.html` and rebuild; the Makefile regenerates the
    embedded C string at build time.

#### App pages `/app` and `/app/*.html`

- **Handler**: `pgstar_route_ui_pages`
- **Mappings**:
  - `/app` or `/app/` → `page_name = 'dashboard'`
  - `/app/<name>.html` → `page_name = '<name>'`
- **Lookup**:
  - Selects from `pgstar.pages`:
    - `SELECT parent_tag, html, content_type, meta, js, css
       FROM pgstar.pages
       WHERE page_name = :name
       ORDER BY is_custom DESC, page_id DESC
       LIMIT 1;`
- **Rendering**:
  - The C code builds a full HTML document:
    - `<title>` derived from `page_name`.
    - `<meta>` tags emitted from `meta` JSON:
      - Objects with `http-equiv` and `content` → `<meta http-equiv="..." content="...">`
      - Objects with `name` and `content` → `<meta name="..." content="...">`
    - For each CSS name in `css`:
      - `<link rel="stylesheet" href="/static/css/<name>">`
    - Body:
      - If `parent_tag = 'main'`, fragment is wrapped in `<main>...</main>`.
      - Otherwise, fragment is written directly into `<body>`.
    - For each JS name in `js`:
      - `<script src="/static/js/<name>"></script>` at the end of `<body>`.

If no row is found for the requested `page_name`, a JSON 404 response is
returned: `{"error":"page not found"}`.

#### JS assets `/static/js/*.js`

- **Handler**: `pgstar_route_ui_js`
- **Mapping**: `/static/js/<name>` → `page_name = 'js/<name>'`
- **Lookup**:
  - `SELECT html, content_type FROM pgstar.pages WHERE page_name = 'js/<name>' ...`
- **Response**:
  - Body: `html` column (or empty string if NULL).
  - `Content-Type`:
    - From row `content_type`, or default `application/javascript`.

If no row is found, returns JSON 404: `{"error":"asset not found"}`.

#### CSS assets `/static/css/*.css`

- **Handler**: `pgstar_route_ui_css`
- **Mapping**: `/static/css/<name>` → `page_name = 'css/<name>'`
- **Lookup**:
  - `SELECT html, content_type FROM pgstar.pages WHERE page_name = 'css/<name>' ...`
- **Response**:
  - Body: `html` column (or empty string if NULL).
  - `Content-Type`:
    - From row `content_type`, or default `text/css; charset=utf-8`.

If no row is found, returns JSON 404: `{"error":"asset not found"}`.

### Caching behaviour (`pgstar.ui_cache_ttl`)

- **GUC**: `pgstar.ui_cache_ttl` (integer seconds, default `5`)
  - Defined in `core/pgstar.c`.
  - `0` disables caching entirely.
- **Scope**:
  - A small in-memory cache (up to 16 entries) keyed by:
    - `page:<page_name>` for HTML pages
    - `js:<name>` for JS assets
    - `css:<name>` for CSS assets
- **Semantics**:
  - On cache hit and non-expired entry:
    - The extension clones the cached body and reuses the cached content type.
  - On miss:
    - The extension queries `pgstar.pages`, renders (for HTML), and stores
      the result with `expires_at = now() + ui_cache_ttl`.
  - Expired entries are lazily invalidated on access.

All caching is per background worker process; there is no cross-backend or
distributed cache.




