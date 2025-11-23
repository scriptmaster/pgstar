#include "postgres.h"

#include "pgstar.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"
#include "core/spi.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

/* Simple in-memory cache for rendered UI assets (HTML/JS/CSS) */

typedef struct PgstarUiCacheEntry
{
	bool		valid;
	char		key[256];
	char	   *body;			/* palloc'd */
	char		content_type[64];
	time_t		expires_at;
} PgstarUiCacheEntry;

static PgstarUiCacheEntry pgstar_ui_cache[16];

static bool pgstar_ui_cache_get(const char *key, char **body, const char **content_type);
static void pgstar_ui_cache_put(const char *key, const char *body, const char *content_type);

static bool pgstar_ui_render_page(const char *page_name, char **body, const char **content_type);
static bool pgstar_ui_fetch_asset(const char *kind, const char *name,
								  char **body, const char **content_type);

static void pgstar_ui_build_html_page(const char *page_name,
									  const char *parent_tag,
									  const char *html_fragment,
									  Jsonb *meta,
									  const char *js_names,
									  const char *css_names,
									  StringInfo buf);
static void pgstar_ui_append_meta_tags(StringInfo head, Jsonb *meta);
static void pgstar_ui_append_css_links(StringInfo head, const char *css_names);
static void pgstar_ui_append_js_scripts(StringInfo body, const char *js_names);
static void pgstar_ui_trim(char *str);

/*
 * Public API: ensure pgstar.pages exists and seed built-in rows.
 */
void
pgstar_ui_ensure_pages(void)
{
	/*
	 * pgstar.pages:
	 *   - page_name: identifier, e.g. 'login', 'dashboard',
	 *                'js/pages_index.js', 'css/pages_index.css'
	 *   - parent_tag: '', 'body', or 'main' for HTML pages
	 *   - html: HTML fragment or full document or asset content
	 *   - content_type: MIME type; defaults to text/html
	 *   - meta: jsonb describing meta tags (optional)
	 *   - js, css: comma-separated logical asset names (e.g. 'pages_index.js')
	 *   - is_custom: when true, seed updates will not override the row
	 */
	pgstar_execute_sql(
		"CREATE TABLE IF NOT EXISTS pgstar.pages ("
		"  page_id      BIGSERIAL PRIMARY KEY,"
		"  page_name    TEXT UNIQUE,"
		"  parent_tag   TEXT NOT NULL DEFAULT '' "
		"               CHECK (parent_tag IN ('','body','main')),"
		"  html         TEXT,"
		"  content_type TEXT NOT NULL "
		"               DEFAULT 'text/html; charset=utf-8',"
		"  meta         JSONB NOT NULL DEFAULT '[]'::jsonb,"
		"  js           TEXT,"
		"  css          TEXT,"
		"  is_custom    BOOLEAN NOT NULL DEFAULT FALSE,"
		"  updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()"
		")");

	/* Default JS bundle asset: /static/js/pages_index.js */
	pgstar_execute_sql(
		"INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, "
		"                          meta, js, css, is_custom) "
		"VALUES ("
		"  'js/pages_index.js',"
		"  '',"
		"  '/* pgstar default JS bundle (datastar loader) */\n"
		"   (function(){\n"
		"     var s = document.createElement(\"script\");\n"
		"     s.src = \"https://cdn.jsdelivr.net/gh/starfederation/datastar@1.0.0-RC.6/bundles/datastar.js\";\n"
		"     document.head.appendChild(s);\n"
		"   })();\n',"
		"  'application/javascript',"
		"  '[]'::jsonb,"
		"  NULL,"
		"  NULL,"
		"  FALSE"
		") "
		"ON CONFLICT (page_name) DO UPDATE "
		"SET html = EXCLUDED.html,"
		"    content_type = EXCLUDED.content_type,"
		"    updated_at = now() "
		"WHERE pgstar.pages.is_custom = FALSE");

	/* Default CSS bundle asset: /static/css/pages_index.css */
	pgstar_execute_sql(
		"INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, "
		"                          meta, js, css, is_custom) "
		"VALUES ("
		"  'css/pages_index.css',"
		"  '',"
		"  '/* pgstar default CSS bundle */\n"
		"   @import url(\"https://static2.sharepointonline.com/files/fabric/office-ui-fabric-core/11.0.0/css/fabric.min.css\");\n"
		"   @import url(\"/static/css/emerald-v2.css\");\n"
		"   @import url(\"https://cdn.jsdelivr.net/npm/@picocss/pico@2.1.1/css/pico.classless.min.css\");\n',"
		"  'text/css; charset=utf-8',"
		"  '[]'::jsonb,"
		"  NULL,"
		"  NULL,"
		"  FALSE"
		") "
		"ON CONFLICT (page_name) DO UPDATE "
		"SET html = EXCLUDED.html,"
		"    content_type = EXCLUDED.content_type,"
		"    updated_at = now() "
		"WHERE pgstar.pages.is_custom = FALSE");

	/* Additional CSS asset: /static/css/emerald-v2.css (placeholder) */
	pgstar_execute_sql(
		"INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, "
		"                          meta, js, css, is_custom) "
		"VALUES ("
		"  'css/emerald-v2.css',"
		"  '',"
		"  '/* pgstar emerald theme placeholder */\n"
		"   :root { --pgstar-accent: #047857; }\n',"
		"  'text/css; charset=utf-8',"
		"  '[]'::jsonb,"
		"  NULL,"
		"  NULL,"
		"  FALSE"
		") "
		"ON CONFLICT (page_name) DO UPDATE "
		"SET html = EXCLUDED.html,"
		"    content_type = EXCLUDED.content_type,"
		"    updated_at = now() "
		"WHERE pgstar.pages.is_custom = FALSE");

	/* Login page: /app/login.html -> page_name 'login' */
	pgstar_execute_sql(
		"INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, "
		"                          meta, js, css, is_custom) "
		"VALUES ("
		"  'login',"
		"  'main',"
		"  '<section>\n"
		"     <h1>Login</h1>\n"
		"     <form data-star>\n"
		"       <label>Username\n"
		"         <input name=\"username\" required />\n"
		"       </label>\n"
		"       <label>Password\n"
		"         <input type=\"password\" name=\"password\" required />\n"
		"       </label>\n"
		"       <button type=\"submit\">Sign in</button>\n"
		"     </form>\n"
		"   </section>\n',"
		"  'text/html; charset=utf-8',"
		"  '[]'::jsonb,"
		"  'pages_index.js',"
		"  'pages_index.css',"
		"  FALSE"
		") "
		"ON CONFLICT (page_name) DO UPDATE "
		"SET parent_tag   = EXCLUDED.parent_tag,"
		"    html         = EXCLUDED.html,"
		"    content_type = EXCLUDED.content_type,"
		"    meta         = EXCLUDED.meta,"
		"    js           = EXCLUDED.js,"
		"    css          = EXCLUDED.css,"
		"    updated_at   = now() "
		"WHERE pgstar.pages.is_custom = FALSE");

	/* Dashboard page: /app/dashboard.html -> page_name 'dashboard' */
	pgstar_execute_sql(
		"INSERT INTO pgstar.pages (page_name, parent_tag, html, content_type, "
		"                          meta, js, css, is_custom) "
		"VALUES ("
		"  'dashboard',"
		"  'main',"
		"  '<section>\n"
		"     <h1>Dashboard</h1>\n"
		"     <p data-star>Welcome to pgstar dashboard.</p>\n"
		"   </section>\n',"
		"  'text/html; charset=utf-8',"
		"  '[]'::jsonb,"
		"  'pages_index.js',"
		"  'pages_index.css',"
		"  FALSE"
		") "
		"ON CONFLICT (page_name) DO UPDATE "
		"SET parent_tag   = EXCLUDED.parent_tag,"
		"    html         = EXCLUDED.html,"
		"    content_type = EXCLUDED.content_type,"
		"    meta         = EXCLUDED.meta,"
		"    js           = EXCLUDED.js,"
		"    css          = EXCLUDED.css,"
		"    updated_at   = now() "
		"WHERE pgstar.pages.is_custom = FALSE");
}

/*
 * Route: /app and /app/*.html
 *
 * Maps:
 *   /app              -> page_name 'dashboard'
 *   /app/             -> page_name 'dashboard'
 *   /app/login.html   -> page_name 'login'
 *   /app/dashboard... -> page_name 'dashboard'
 */
void
pgstar_route_ui_pages(const PgstarRequest *req, PgstarResponse *resp)
{
	char		page_name[128];
	const char *path = req->path;
	char	   *body = NULL;
	const char *content_type = NULL;
	char		cache_key[192];

	if (pg_strcasecmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	memset(page_name, 0, sizeof(page_name));

	if (strcmp(path, "/app") == 0 || strcmp(path, "/app/") == 0)
	{
		strlcpy(page_name, "dashboard", sizeof(page_name));
	}
	else if (strncmp(path, "/app/", 5) == 0)
	{
		const char *suffix = path + 5;
		size_t		len;

		if (*suffix == '\0')
		{
			strlcpy(page_name, "dashboard", sizeof(page_name));
		}
		else
		{
			const char *slash = strchr(suffix, '/');

			/* Do not allow nested paths like /app/a/b */
			if (slash != NULL)
			{
				resp->status_code = 404;
				resp->status_text = "Not Found";
				resp->content_type = "application/json";
				resp->body = pstrdup("{\"error\":\"not found\"}\n");
				return;
			}

			len = strlen(suffix);
			if (len >= 5 && pg_strcasecmp(suffix + len - 5, ".html") == 0)
			{
				len -= 5;
			}

			if (len == 0 || len >= sizeof(page_name))
			{
				resp->status_code = 404;
				resp->status_text = "Not Found";
				resp->content_type = "application/json";
				resp->body = pstrdup("{\"error\":\"not found\"}\n");
				return;
			}

			memcpy(page_name, suffix, len);
			page_name[len] = '\0';
		}
	}
	else
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	snprintf(cache_key, sizeof(cache_key), "page:%s", page_name);

	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		return;
	}

	if (!pgstar_ui_render_page(page_name, &body, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"page not found\"}\n");
		return;
	}

	pgstar_ui_cache_put(cache_key, body, content_type);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = content_type;
	resp->body = body;
}

/*
 * Route: /static/js/*.js
 * Uses pgstar.pages rows with page_name 'js/<name>.js'.
 */
void
pgstar_route_ui_js(const PgstarRequest *req, PgstarResponse *resp)
{
	const char *path = req->path;
	const char *name;
	char		cache_key[192];
	char	   *body = NULL;
	const char *content_type = NULL;

	if (pg_strcasecmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	if (strncmp(path, "/static/js/", 11) != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	name = path + 11;
	if (*name == '\0' || strchr(name, '/') != NULL)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	snprintf(cache_key, sizeof(cache_key), "js:%s", name);

	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		return;
	}

	if (!pgstar_ui_fetch_asset("js", name, &body, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"asset not found\"}\n");
		return;
	}

	pgstar_ui_cache_put(cache_key, body, content_type);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = content_type;
	resp->body = body;
}

/*
 * Route: /static/css/*.css
 * Uses pgstar.pages rows with page_name 'css/<name>.css'.
 */
void
pgstar_route_ui_css(const PgstarRequest *req, PgstarResponse *resp)
{
	const char *path = req->path;
	const char *name;
	char		cache_key[192];
	char	   *body = NULL;
	const char *content_type = NULL;

	if (pg_strcasecmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	if (strncmp(path, "/static/css/", 12) != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	name = path + 12;
	if (*name == '\0' || strchr(name, '/') != NULL)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	snprintf(cache_key, sizeof(cache_key), "css:%s", name);

	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		return;
	}

	if (!pgstar_ui_fetch_asset("css", name, &body, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"asset not found\"}\n");
		return;
	}

	pgstar_ui_cache_put(cache_key, body, content_type);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = content_type;
	resp->body = body;
}

/* -------------------------------------------------------------------------
 * Cache helpers
 * -------------------------------------------------------------------------
 */
static bool
pgstar_ui_cache_get(const char *key, char **body, const char **content_type)
{
	time_t		now;
	int			i;

	if (pgstar_ui_cache_ttl <= 0)
		return false;

	now = time(NULL);

	for (i = 0; i < (int) lengthof(pgstar_ui_cache); i++)
	{
		PgstarUiCacheEntry *e = &pgstar_ui_cache[i];

		if (!e->valid)
			continue;

		if (e->expires_at <= now)
		{
			e->valid = false;
			continue;
		}

		if (strcmp(e->key, key) == 0)
		{
			*body = pstrdup(e->body);
			*content_type = e->content_type;
			return true;
		}
	}

	return false;
}

static void
pgstar_ui_cache_put(const char *key, const char *body, const char *content_type)
{
	time_t		now;
	int			i;
	int			slot = -1;
	time_t		oldest = 0;

	if (pgstar_ui_cache_ttl <= 0)
		return;

	now = time(NULL);

	for (i = 0; i < (int) lengthof(pgstar_ui_cache); i++)
	{
		PgstarUiCacheEntry *e = &pgstar_ui_cache[i];

		if (!e->valid)
		{
			slot = i;
			break;
		}

		if (!e->valid || e->expires_at <= now)
		{
			slot = i;
			break;
		}

		if (slot < 0 || e->expires_at < oldest)
		{
			slot = i;
			oldest = e->expires_at;
		}
	}

	if (slot < 0)
		return;

	pgstar_ui_cache[slot].valid = true;
	strlcpy(pgstar_ui_cache[slot].key, key, sizeof(pgstar_ui_cache[slot].key));

	if (pgstar_ui_cache[slot].body)
		pfree(pgstar_ui_cache[slot].body);
	pgstar_ui_cache[slot].body = pstrdup(body);

	strlcpy(pgstar_ui_cache[slot].content_type, content_type,
			sizeof(pgstar_ui_cache[slot].content_type));
	pgstar_ui_cache[slot].expires_at = now + pgstar_ui_cache_ttl;
}

/* -------------------------------------------------------------------------
 * Database helpers
 * -------------------------------------------------------------------------
 */
static bool
pgstar_ui_render_page(const char *page_name, char **body, const char **content_type)
{
	StringInfoData buf;
	int			ret;
	bool		found = false;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errmsg("pgstar ui: SPI_connect failed: %d", ret)));

	PG_TRY();
	{
		Oid			argtypes[1];
		Datum		values[1];

		argtypes[0] = TEXTOID;
		values[0] = CStringGetTextDatum(page_name);

		ret = SPI_execute_with_args(
			"SELECT parent_tag, html, content_type, meta, js, css "
			"FROM pgstar.pages "
			"WHERE page_name = $1 "
			"ORDER BY is_custom DESC, page_id DESC "
			"LIMIT 1",
			1,
			argtypes,
			values,
			NULL,
			true,
			1);

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_execute_with_args failed: %d", ret)));

		if (SPI_processed == 1)
		{
			HeapTuple	tup = SPI_tuptable->vals[0];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			char	   *parent_tag;
			char	   *html_fragment;
			char	   *ct;
			Datum		meta_datum;
			bool		isnull;
			Jsonb	   *meta = NULL;
			char	   *js_names;
			char	   *css_names;

			parent_tag = SPI_getvalue(tup, tupdesc, 1);
			html_fragment = SPI_getvalue(tup, tupdesc, 2);
			ct = SPI_getvalue(tup, tupdesc, 3);

			meta_datum = SPI_getbinval(tup, tupdesc, 4, &isnull);
			if (!isnull)
				meta = DatumGetJsonbP(meta_datum);

			js_names = SPI_getvalue(tup, tupdesc, 5);
			css_names = SPI_getvalue(tup, tupdesc, 6);

			if (ct == NULL)
				ct = pstrdup("text/html; charset=utf-8");

			initStringInfo(&buf);
			pgstar_ui_build_html_page(page_name,
									  parent_tag ? parent_tag : "",
									  html_fragment ? html_fragment : "",
									  meta,
									  js_names,
									  css_names,
									  &buf);

			*body = buf.data;
			*content_type = pstrdup(ct);
			found = true;
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}
	PG_END_TRY();

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return found;
}

static bool
pgstar_ui_fetch_asset(const char *kind, const char *name,
					  char **body, const char **content_type)
{
	StringInfoData buf;
	int			ret;
	bool		found = false;
	char		page_name[256];

	snprintf(page_name, sizeof(page_name), "%s/%s", kind, name);

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errmsg("pgstar ui: SPI_connect failed: %d", ret)));

	PG_TRY();
	{
		Oid			argtypes[1];
		Datum		values[1];

		argtypes[0] = TEXTOID;
		values[0] = CStringGetTextDatum(page_name);

		ret = SPI_execute_with_args(
			"SELECT html, content_type "
			"FROM pgstar.pages "
			"WHERE page_name = $1 "
			"ORDER BY is_custom DESC, page_id DESC "
			"LIMIT 1",
			1,
			argtypes,
			values,
			NULL,
			true,
			1);

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_execute_with_args failed: %d", ret)));

		if (SPI_processed == 1)
		{
			HeapTuple	tup = SPI_tuptable->vals[0];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			char	   *content;
			char	   *ct;

			content = SPI_getvalue(tup, tupdesc, 1);
			ct = SPI_getvalue(tup, tupdesc, 2);

			if (content == NULL)
				content = "";

			if (ct == NULL)
			{
				if (strcmp(kind, "js") == 0)
					ct = pstrdup("application/javascript");
				else
					ct = pstrdup("text/css; charset=utf-8");
			}

			initStringInfo(&buf);
			appendStringInfoString(&buf, content);

			*body = buf.data;
			*content_type = pstrdup(ct);
			found = true;
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}
	PG_END_TRY();

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return found;
}

/* -------------------------------------------------------------------------
 * HTML helpers
 * -------------------------------------------------------------------------
 */
static void
pgstar_ui_build_html_page(const char *page_name,
						  const char *parent_tag,
						  const char *html_fragment,
						  Jsonb *meta,
						  const char *js_names,
						  const char *css_names,
						  StringInfo buf)
{
	bool		use_main = (parent_tag && strcmp(parent_tag, "main") == 0);

	appendStringInfoString(buf, "<!doctype html>\n<html lang=\"en\">\n<head>\n");
	appendStringInfoString(buf, "  <meta charset=\"utf-8\">\n");
	appendStringInfo(buf, "  <title>%s</title>\n",
					 page_name ? page_name : "pgstar");

	if (meta != NULL)
		pgstar_ui_append_meta_tags(buf, meta);

	pgstar_ui_append_css_links(buf, css_names);

	appendStringInfoString(buf, "</head>\n<body>\n");

	if (use_main)
	{
		appendStringInfoString(buf, "<main>\n");
		appendStringInfoString(buf, html_fragment);
		appendStringInfoString(buf, "\n</main>\n");
	}
	else
	{
		appendStringInfoString(buf, html_fragment);
		appendStringInfoString(buf, "\n");
	}

	pgstar_ui_append_js_scripts(buf, js_names);

	appendStringInfoString(buf, "\n</body>\n</html>\n");
}

static void
pgstar_ui_append_meta_tags(StringInfo head, Jsonb *meta)
{
	JsonbIterator *it;
	JsonbValue	v;
	int			r;
	bool		in_object = false;
	char	   *current_key = NULL;
	char	   *http_equiv = NULL;
	char	   *name = NULL;
	char	   *content = NULL;

	if (meta == NULL)
		return;

	it = JsonbIteratorInit(&meta->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (r == WJB_BEGIN_OBJECT)
		{
			in_object = true;
			if (http_equiv)
				pfree(http_equiv);
			if (name)
				pfree(name);
			if (content)
				pfree(content);
			http_equiv = name = content = NULL;
		}
		else if (r == WJB_END_OBJECT)
		{
			if (in_object)
			{
				if (http_equiv && content)
				{
					appendStringInfo(head,
									 "  <meta http-equiv=\"%s\" content=\"%s\">\n",
									 http_equiv,
									 content);
				}
				else if (name && content)
				{
					appendStringInfo(head,
									 "  <meta name=\"%s\" content=\"%s\">\n",
									 name,
									 content);
				}
			}
			in_object = false;
		}
		else if (r == WJB_KEY && v.type == jbvString)
		{
			if (current_key)
			{
				pfree(current_key);
				current_key = NULL;
			}
			current_key = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (r == WJB_VALUE && current_key != NULL)
		{
			if (v.type == jbvString)
			{
				char   *val = pnstrdup(v.val.string.val, v.val.string.len);

				if (pg_strcasecmp(current_key, "http-equiv") == 0)
				{
					if (http_equiv)
						pfree(http_equiv);
					http_equiv = val;
				}
				else if (pg_strcasecmp(current_key, "name") == 0)
				{
					if (name)
						pfree(name);
					name = val;
				}
				else if (pg_strcasecmp(current_key, "content") == 0)
				{
					if (content)
						pfree(content);
					content = val;
				}
				else
				{
					pfree(val);
				}
			}

			pfree(current_key);
			current_key = NULL;
		}
	}

	if (current_key)
		pfree(current_key);
	if (http_equiv)
		pfree(http_equiv);
	if (name)
		pfree(name);
	if (content)
		pfree(content);
}

static void
pgstar_ui_append_css_links(StringInfo head, const char *css_names)
{
	char	   *work;
	char	   *token;
	char	   *saveptr = NULL;

	if (css_names == NULL || css_names[0] == '\0')
		return;

	work = pstrdup(css_names);
	pgstar_ui_trim(work);

	for (token = strtok_r(work, ",", &saveptr);
		 token != NULL;
		 token = strtok_r(NULL, ",", &saveptr))
	{
		pgstar_ui_trim(token);
		if (token[0] == '\0')
			continue;

		appendStringInfo(head,
						 "  <link rel=\"stylesheet\" href=\"/static/css/%s\">\n",
						 token);
	}

	pfree(work);
}

static void
pgstar_ui_append_js_scripts(StringInfo body, const char *js_names)
{
	char	   *work;
	char	   *token;
	char	   *saveptr = NULL;

	if (js_names == NULL || js_names[0] == '\0')
		return;

	work = pstrdup(js_names);
	pgstar_ui_trim(work);

	for (token = strtok_r(work, ",", &saveptr);
		 token != NULL;
		 token = strtok_r(NULL, ",", &saveptr))
	{
		pgstar_ui_trim(token);
		if (token[0] == '\0')
			continue;

		appendStringInfo(body,
						 "  <script src=\"/static/js/%s\"></script>\n",
						 token);
	}

	pfree(work);
}

static void
pgstar_ui_trim(char *str)
{
	char   *start = str;
	char   *end;

	if (str == NULL)
		return;

	while (*start && isspace((unsigned char) *start))
		start++;

	if (start != str)
		memmove(str, start, strlen(start) + 1);

	end = str + strlen(str);
	while (end > str && isspace((unsigned char) *(end - 1)))
		end--;

	*end = '\0';
}


