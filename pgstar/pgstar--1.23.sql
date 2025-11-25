-- Install script for the pgstar extension

CREATE OR REPLACE FUNCTION pgstar_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_version'
LANGUAGE C
STRICT
STABLE;

CREATE OR REPLACE FUNCTION pgstar_http_post(
	url text,
	headers jsonb,
	body text
)
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_http_post'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstar_http_get(
	url text,
	headers jsonb
)
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_http_get'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstart_create_job(
	job_type text,
	cron_expression text,
	table_name text,
	key_column text,
	given_stmt text,
	parameters jsonb,
	range_direction text,
	start_range numeric,
	end_range numeric DEFAULT NULL,
	batch_count integer DEFAULT 1000
)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgstart_create_job'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstar_job_status(
	job_id bigint
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'pgstar_job_status'
LANGUAGE C
STABLE;

-- Create pgstar schema and pages table synchronously during extension creation
-- This ensures the table exists immediately, even before the background worker initializes
CREATE SCHEMA IF NOT EXISTS pgstar;

CREATE TABLE IF NOT EXISTS pgstar.pages (
  page_id      BIGSERIAL PRIMARY KEY,
  page_name    TEXT UNIQUE,
  parent_tag   TEXT NOT NULL DEFAULT ''
               CHECK (parent_tag IN ('','body','main')),
  html         TEXT,
  content_type TEXT NOT NULL 
               DEFAULT 'text/html; charset=utf-8',
  meta         JSONB NOT NULL DEFAULT '[]'::jsonb,
  js           TEXT,
  css          TEXT,
  is_custom    BOOLEAN NOT NULL DEFAULT FALSE,
  updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Insert seed data synchronously during extension creation
-- Note: These use ON CONFLICT to update only non-custom pages, preserving user data
INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'js/pages_index.js',
  '',
  '/* pgstar default JS bundle (datastar loader) */
',
  'application/javascript',
  '[]'::jsonb,
  NULL,
  NULL,
  FALSE
)
ON CONFLICT (page_name) DO UPDATE
SET html = EXCLUDED.html,
    content_type = EXCLUDED.content_type,
    updated_at = now()
WHERE pgstar.pages.is_custom = FALSE;

INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'css/pages_index.css',
  '',
  '/* pgstar default CSS bundle */
   @import url("https://static2.sharepointonline.com/files/fabric/office-ui-fabric-core/11.0.0/css/fabric.min.css");
   @import url("/static/css/emerald-v2.css");
   @import url("https://cdn.jsdelivr.net/npm/@picocss/pico@2.1.1/css/pico.classless.min.css");
',
  'text/css; charset=utf-8',
  '[]'::jsonb,
  NULL,
  NULL,
  FALSE
)
ON CONFLICT (page_name) DO UPDATE
SET html = EXCLUDED.html,
    content_type = EXCLUDED.content_type,
    updated_at = now()
WHERE pgstar.pages.is_custom = FALSE;

INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'css/emerald-v2.css',
  '',
  '/* pgstar emerald theme placeholder */
   :root { --pgstar-accent: #047857; }
',
  'text/css; charset=utf-8',
  '[]'::jsonb,
  NULL,
  NULL,
  FALSE
)
ON CONFLICT (page_name) DO UPDATE
SET html = EXCLUDED.html,
    content_type = EXCLUDED.content_type,
    updated_at = now()
WHERE pgstar.pages.is_custom = FALSE;

INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'login',
  'main',
  '<section>
     <h1>Login</h1>
     <form data-star>
       <label>Username
         <input name="username" required />
       </label>
       <label>Password
         <input type="password" name="password" required />
       </label>
       <button type="submit">Sign in</button>
     </form>
   </section>
',
  'text/html; charset=utf-8',
  '[]'::jsonb,
  '',
  '',
  FALSE
)
ON CONFLICT (page_name) DO UPDATE
SET parent_tag   = EXCLUDED.parent_tag,
    html         = EXCLUDED.html,
    content_type = EXCLUDED.content_type,
    meta         = EXCLUDED.meta,
    js           = EXCLUDED.js,
    css          = EXCLUDED.css,
    updated_at   = now()
WHERE pgstar.pages.is_custom = FALSE;

-- DELETE FROM pgstar.pages WHERE page_name='dashboard';
INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'dashboard',
  '',
  '<section class="panel" hx-ext="client-side-templates">
    <aside>
      <ul
        class="tree-view has-connector has-collapse-button has-container has-scrollbar"
      >
        <li><a href="/app/">Dashboard</a></li>
        <li>
          <a href="/app/tables">Tables</a>
          <ul>
            <li><a href="/app/tables/users">Users</a></li>
            <li><a href="/app/tables/users">Jobs</a></li>
          </ul>
        </li>
      </ul>
    </aside>
    <main>
      <section class="titlebar">
        <ul role="menubar">
          <li role="menuitem" tabindex="0">
            List
            <ul role="menu">
              <li role="menuitem" tabindex="0" aria-haspopup="true">
                View Columns
                <ul role="menu">
                  <li role="menuitem"><input type="checkbox" id="col1" checked><label for="col1">id</label></li>
                </ul>
              </li>
              <li role="menuitem" tabindex="0">
                <a href="#first-page">First Page</a></li>
              <li role="menuitem" tabindex="0">
                <a href="#prev-page">Previous Page</a></li>
              <li role="menuitem" tabindex="0">
                <a href="#next-page">Next Page</a></li>
              <li role="menuitem" tabindex="0">
                <a href="#last-page">Last Page</a></li>
              <li role="menuitem" aria-haspopup="true">Page Number: 1<ul role="menu"></ul></li>
              <li role="menuitem" aria-haspopup="true">
                Page Size: 25
                <ul role="menu">
                  <li role="menuitem">
                    <a href="#page-size-25">25</a></li>
                  <li role="menuitem">
                    <a href="#page-size-100">100</a></li>
                  <li role="menuitem">
                    <a href="#page-size-500">500</a></li>
                  <li role="menuitem">
                    <a href="#page-size-1000">1000</a></li>
                </ul>
              </li>
            </ul>
          </li>
          <li role="menuitem" tabindex="0">Create</li>
          <li role="menuitem" tabindex="0">Read</li>
          <li role="menuitem" tabindex="0">Update</li>
          <li role="menuitem" tabindex="0">Delete</li>
          <li role="menuitem" tabindex="0">Options</li>
          <div class="searchbox mt-1">
            <input type="search" placeholder="Filter" />
            <button aria-label="search"></button>
          </div>
        </ul>
      </section>
      <div class="window-body m-0">
        <table>
          <tr>
            <th>id</th>
            <th>name</th>
          </tr>
        </table>
      </div>
    </main>
   </section>
',
  'text/html; charset=utf-8',
  '[]'::jsonb,
  '',
  -- 'https://unpkg.com/htmx.org@2.0.8/dist/htmx.min.js,https://unpkg.com/htmx.org@1.9.12/dist/ext/client-side-templates.js,https://unpkg.com/mustache@latest/mustache.min.js',
  '',
  FALSE
)
ON CONFLICT (page_name) DO UPDATE
SET parent_tag   = EXCLUDED.parent_tag,
    html         = EXCLUDED.html,
    content_type = EXCLUDED.content_type,
    meta         = EXCLUDED.meta,
    js           = EXCLUDED.js,
    css          = EXCLUDED.css,
    updated_at   = now()
WHERE pgstar.pages.is_custom = FALSE;

