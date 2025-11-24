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

DELETE FROM pgstar.pages WHERE page_name='dashboard';
INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'dashboard',
  'main',
  '<section>
     <h1>PGSTAR Dashboard!</h1>
     <p data-star>Welcome to pgstar dashboard.</p>
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

