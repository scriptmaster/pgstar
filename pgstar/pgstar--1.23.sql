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

CREATE TABLE IF NOT EXISTS pgstar.files (
  file_id       BIGSERIAL PRIMARY KEY,
  file_name     TEXT UNIQUE NOT NULL,
  content_type  TEXT NOT NULL
                DEFAULT 'application/octet-stream',
  text_data     TEXT,
  download_url  TEXT,
  is_binary     BOOLEAN NOT NULL DEFAULT FALSE,
  is_custom     BOOLEAN NOT NULL DEFAULT FALSE,
  is_download   BOOLEAN NOT NULL DEFAULT FALSE,
  updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (
    (is_download = TRUE AND download_url IS NOT NULL) OR
    (is_download = FALSE AND text_data IS NOT NULL)
  )
);

-- Provision public.users so /api/public/users succeeds out of the box
-- Ensure public schema exists; some cloud DBs drop it
-- Migrate JS/CSS files from pages to files table
INSERT INTO pgstar.files (file_name, content_type, text_data, is_binary, is_custom)
SELECT page_name, content_type, html, FALSE, is_custom
FROM pgstar.pages
WHERE page_name LIKE 'js/%' OR page_name LIKE 'css/%'
ON CONFLICT (file_name) DO UPDATE
SET text_data = EXCLUDED.text_data,
    content_type = EXCLUDED.content_type,
    updated_at = now()
WHERE pgstar.files.is_custom = FALSE;

-- Add Vue.js reference (stored as URL in text_data for now)
INSERT INTO pgstar.files (file_name, content_type, download_url, is_binary, is_download, is_custom)
VALUES (
  'js/vue.global.prod.min.js',
  'application/javascript',
  'https://cdnjs.cloudflare.com/ajax/libs/vue/3.5.22/vue.global.prod.min.js',
  FALSE,
  TRUE,
  FALSE
)
ON CONFLICT (file_name) DO UPDATE
SET download_url = EXCLUDED.download_url,
    text_data = NULL,
    content_type = EXCLUDED.content_type,
    is_download = EXCLUDED.is_download,
    is_binary = EXCLUDED.is_binary,
    updated_at = now()
WHERE pgstar.files.is_custom = FALSE;

-- Add Vue data-table component
INSERT INTO pgstar.files (file_name, content_type, text_data, is_binary, is_custom)
VALUES (
  'js/vue-data-table.js',
  'application/javascript',
  $$const { createApp } = Vue;

const app = createApp({
    computed: {
        currentTableSrc() {
            const path = window.location.pathname;
            if (path.endsWith('/files')) return '/api/files';
            if (path.endsWith('/users')) return '/api/pgstar/users';
            if (path.endsWith('/jobs')) return '/api/jobs';
            // Default to information schema tables
            return '/api/information_schema/tables';
        }
    }
});

// Register component with name that works with is attribute
app.component('data-table-grid', {
    template: '#data-table-template',
    props: ['src'],
    
    data() {
        return {
            rows: [],
            loading: true,
            error: null
        }
    },
    computed: {
        // Dynamically grab keys from the first object to form columns
        columns() {
            if (this.rows.length === 0) return [];
            return Object.keys(this.rows[0]);
        }
    },
    methods: {
        async fetchData() {
            console.log('Fetching data from:', this.src);
            this.loading = true;
            this.error = null;
            try {
                const res = await fetch(this.src);
                if (!res.ok) throw new Error(res.statusText);
                const data = await res.json();
                console.log('Data received:', data);
                // Handle both array and object with rows property
                if (Array.isArray(data)) {
                    this.rows = data;
                } else if (data && Array.isArray(data.rows)) {
                    this.rows = data.rows;
                } else {
                    this.rows = [];
                }
            } catch (err) {
                console.error('Fetch error:', err);
                this.error = err.message;
            } finally {
                this.loading = false;
            }
        },
        // Utility: Check if a value implies nested data (JSON object)
        isObject(val) {
            return typeof val === 'object' && val !== null;
        },
        // Utility: Convert "camelCase" or "snake_case" to "Title Case"
        humanize(str) {
            return str
                // Handle snake_case: replace underscores with spaces
                .replace(/_/g, ' ')
                // Handle camelCase: add space before capital letters
                .replace(/([A-Z])/g, ' $1')
                // Capitalize first letter of each word
                .split(' ')
                .map(word => word.charAt(0).toUpperCase() + word.slice(1).toLowerCase())
                .join(' ')
                .trim();
        }
    },
    mounted() {
        console.log('DataTableGrid mounted, src:', this.src);
        this.fetchData();
    },
    // Watch for src changes to re-fetch
    watch: {
        src(newSrc) {
            console.log('Source changed to:', newSrc);
            this.fetchData();
        }
    }
});

app.mount('#app');
console.log('Vue app mounted');
$$,
  FALSE,
  FALSE
)
ON CONFLICT (file_name) DO UPDATE
SET text_data = EXCLUDED.text_data,
    content_type = EXCLUDED.content_type,
    updated_at = now()
WHERE pgstar.files.is_custom = FALSE;

-- Add default favicon (lazy download)
INSERT INTO pgstar.files (file_name, content_type, download_url, is_binary, is_download, is_custom)
VALUES (
  'favicon.ico',
  'image/x-icon',
  'https://emeraldai.cloud/favicon.ico',
  FALSE,
  TRUE,
  FALSE
)
ON CONFLICT (file_name) DO UPDATE
SET download_url = EXCLUDED.download_url,
    text_data = NULL,
    content_type = EXCLUDED.content_type,
    is_download = EXCLUDED.is_download,
    is_binary = EXCLUDED.is_binary,
    updated_at = now()
WHERE pgstar.files.is_custom = FALSE;

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
  'js/emerald-star.js',
  '',
  $$// Emerald Star: Custom datastar plugins from emerald

// datastar-json-table.plugin.js
// Datastar custom plugin for:
// <table data-json-table="/api/..."
//        data-json-rows="rows"           (optional)
//        data-json-table-columns="#cols" (optional)>

// Wait for datastar to be available and register the plugin
(function() {
  function registerAttribute() {
    // Try to get attribute function from various possible locations
    // Datastar may expose it globally or through window.datastar
    let attr = null;
    
    // Check global scope first (if datastar exposes it globally)
    if (typeof attribute !== 'undefined' && typeof attribute === 'function') {
      attr = attribute;
    }
    // Check window.datastar
    else if (window.datastar && typeof window.datastar.attribute === 'function') {
      attr = window.datastar.attribute;
    }
    // Check window.Datastar
    else if (window.Datastar && typeof window.Datastar.attribute === 'function') {
      attr = window.Datastar.attribute;
    }
    // Check if datastar module has loaded and exposed it
    else if (window.__datastar__ && typeof window.__datastar__.attribute === 'function') {
      attr = window.__datastar__.attribute;
    }
    
    if (!attr) {
      // Retry after a short delay if datastar is not loaded yet
      setTimeout(registerAttribute, 100);
      return;
    }

    // Register the custom attribute
    attr({
  name: 'json-table',
  requirement: {
    key: 'denied',  // use as data-json-table="..."
    value: 'must',  // value (URL or expression) is required
  },
  returnsValue: false,

  apply({ el, value, rx }) {
    // Resolve URL: plugin value, expression, or raw attribute
    let url = value;
    if (!url) {
      try {
        const maybe = rx();
        if (typeof maybe === 'string' && maybe.length) {
          url = maybe;
        }
      } catch (_) {
        /* ignore expression errors */
      }
    }
    if (!url) {
      url = el.getAttribute('data-json-table');
    }
    if (!url) return;

    // Optional key for rows array: data-json-rows="rows"
    // If this is empty or missing, we expect a top-level array.
    const rowsKeyAttr = el.getAttribute('data-json-rows');
    const rowsKey =
      rowsKeyAttr !== null && rowsKeyAttr !== '' ? rowsKeyAttr : null;

    // Optional column selector container
    const columnsSelector = el.getAttribute('data-json-table-columns') || null;
    const columnsContainer = columnsSelector
      ? document.querySelector(columnsSelector)
      : null;

    const state = {
      el,
      url,
      rowsKey,
      rows: [],
      columnsContainer,
      listener: null,
    };

    // React to column checkbox changes
    if (columnsContainer) {
      const handler = () => {
        if (!state.rows.length) return;
        renderTable(state);
      };
      columnsContainer.addEventListener('change', handler);
      state.listener = handler;
    }

    // Initial fetch
    fetchJsonAndRender(state);

    // Cleanup on unmount / patch
    return () => {
      if (state.columnsContainer && state.listener) {
        state.columnsContainer.removeEventListener(
          'change',
          state.listener,
        );
      }
    };
  },
    }); // Close attr() call
  }
  
  // Start registration when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', registerAttribute);
  } else {
    registerAttribute();
  }
})(); // Close IIFE

async function fetchJsonAndRender(state) {
  const { url, el, rowsKey } = state;

  try {
    const res = await fetch(url, {
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}`);
    }

    const json = await res.json();
    let rows = [];

    if (rowsKey) {
      // Explicit rows key: { [rowsKey]: [...] }
      if (
        json &&
        typeof json === 'object' &&
        Array.isArray(json[rowsKey])
      ) {
        rows = json[rowsKey];
      }
    } else {
      // No rowsKey specified:
      // 1) Prefer top-level array
      // 2) Fallback to json.rows if present
      if (Array.isArray(json)) {
        rows = json;
      } else if (
        typeof json === 'object' &&
        typeof json.rows == 'object' &&
        Array.isArray(json.rows)
      ) {
        rows = json.rows;
      }
    }

    state.rows = rows;

    if (!rows.length) {
      renderEmpty(el, 'No rows');
      return;
    }

    renderTable(state);
  } catch (err) {
    console.error('json-table plugin fetch error:', err);
    renderEmpty(el, 'Failed to load');
  }
}

function renderEmpty(tableEl, message) {
  tableEl.innerHTML = `
    <thead><tr><th></th></tr></thead>
    <tbody><tr><td colspan="99">${escapeHtml(message)}</td></tr></tbody>
  `;
}

function renderTable(state) {
  const { el, rows, columnsContainer } = state;

  if (!rows || !rows.length) {
    renderEmpty(el, 'No rows');
    return;
  }

  const visibleColumns = getVisibleColumns(rows, columnsContainer);
  if (!visibleColumns.length) {
    renderEmpty(el, 'No visible columns');
    return;
  }

  const theadHtml =
    '<thead><tr>' +
    visibleColumns.map((c) => `<th>${escapeHtml(c)}</th>`).join('') +
    '</tr></thead>';

  const bodyHtml =
    '<tbody>' +
    rows
      .map((row) => {
        const cells = getRowCells(row, visibleColumns);
        const tds = cells
          .map((cell) => `<td>${escapeHtml(cell)}</td>`)
          .join('');
        return `<tr>${tds}</tr>`;
      })
      .join('') +
    '</tbody>';

  el.innerHTML = theadHtml + bodyHtml;
}

function getVisibleColumns(rows, columnsContainer) {
  // If we have a "columns signal" container, use checked checkboxes
  if (columnsContainer) {
    const inputs = columnsContainer.querySelectorAll(
      'input[type="checkbox"][data-column]',
    );
    const cols = [];
    inputs.forEach((input) => {
      if (input.checked) cols.push(input.dataset.column);
    });
    if (cols.length) return cols;
  }

  // Fallback: derive from keys of first row (for object rows)
  const first = rows[0];
  if (first && typeof first === 'object' && !Array.isArray(first)) {
    return Object.keys(first);
  }

  return [];
}

function getRowCells(row, visibleColumns) {
  if (Array.isArray(row)) return row;

  if (row && typeof row === 'object') {
    return visibleColumns.map((col) => row[col]);
  }

  return [];
}

function escapeHtml(v) {
  return String(v ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&lt;')
    .replace(/</g, '&lt;') // small redundancy, harmless
    .replace(/>/g, '&gt;')
    .replace(new RegExp(String.fromCharCode(34), 'g'), '&quot;') // double quotes
    .replace(new RegExp(String.fromCharCode(39), 'g'), '&#39;'); // single quotes
}
$$,
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

-- Ensure clean reinstall of dashboard page
DELETE FROM pgstar.pages WHERE page_name = 'dashboard' AND is_custom = FALSE;
INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, meta, js, css, is_custom)
VALUES (
  'dashboard',
  '',
  '<div id="app">
    <section class="panel">
    <aside>
        <ul class="tree-view has-connector has-collapse-button has-container has-scrollbar">
        <li><a href="/app/">Dashboard</a></li>
        <li>
          <a href="/app/tables">Tables</a>
          <ul>
            <li><a href="/app/tables/users">Users</a></li>
              <li><a href="/app/tables/files">Files</a></li>
              <li><a href="/app/tables/jobs">Jobs</a></li>
          </ul>
        </li>
      </ul>
    </aside>
    <main>
      <section class="titlebar">
        <ul role="menubar">
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
          <data-table-grid :src="currentTableSrc"></data-table-grid>
        </div>
      </main>
    </section>
  </div>
    
  <template id="data-table-template">
      <div v-if="loading">
        <p>Loading data...</p>
      </div>
      <div v-else-if="error" class="error">Error: {{ error }}</div>
      <table v-else>
        <thead>
          <tr>
            <th v-for="col in columns" :key="col">{{ humanize(col) }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(row, idx) in rows" :key="idx">
            <td v-for="col in columns" :key="col">
              <span v-if="!isObject(row[col])">{{ row[col] }}</span>
              <pre v-else>{{ JSON.stringify(row[col], null, 2) }}</pre>
            </td>
          </tr>
        </tbody>
        </table>
    </template>
',
  'text/html; charset=utf-8',
  '[]'::jsonb,
  'vue.global.prod.min.js,vue-data-table.js',
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

