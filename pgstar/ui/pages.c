#include "postgres.h"

#include "pgstar.h"
#include "pages_layout.h"

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

/*
 * Public API: ensure pgstar.pages exists and seed built-in rows.
 * 
 * MIGRATED TO SQL: This function's table creation and seed data insertion
 * has been migrated to pgstar--1.20.sql (and future version files) to ensure
 * synchronous creation during CREATE EXTENSION. The table and seed data are
 * now created immediately when the extension is installed, eliminating race
 * conditions with the background worker initialization.
 * 
 * This function is kept as a no-op stub for backward compatibility.
 * If needed in the future for dynamic updates or important runtime initialization,
 * the implementation can be restored here.
 */
void
pgstar_ui_ensure_pages(void)
{
	/* No-op: table creation and seed data now handled in pgstar--*.sql files */
	/* 
	 * Previous implementation created pgstar.pages table and inserted seed data:
	 * - Table structure with page_id, page_name, parent_tag, html, content_type, etc.
	 * - Seed data for: js/pages_index.js, css/pages_index.css, css/emerald-v2.css,
	 *   login page, dashboard page, and other default pages
	 * 
	 * All of this is now in pgstar--1.20.sql for synchronous execution during
	 * CREATE EXTENSION.
	 */
}

/*
 * Route: /app and /app/X.html
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
	char		default_page_name[128];
	const char *path = req->path;
	char	   *body = NULL;
	const char *content_type = NULL;
	char		cache_key[192];

	elog(pgstar_log_level, "pgstar: UI pages request: %s %s", req->method, req->path);

	if (pg_strcasecmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	memset(page_name, 0, sizeof(page_name));

	strlcpy(default_page_name, "dashboard", sizeof(default_page_name));

	if (strcmp(path, "/app") == 0 || strcmp(path, "/app/") == 0)
	{
		strlcpy(page_name, default_page_name, sizeof(page_name));
	}
	else if (strncmp(path, "/app/", 5) == 0)
	{
		const char *suffix = path + 5;
		size_t		len;

		if (*suffix == '\0')
		{
			strlcpy(page_name, default_page_name, sizeof(page_name));
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
		elog(pgstar_log_level, "pgstar: UI cache HIT for page '%s' (key: %s)", page_name, cache_key);
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		/*
		 * Take our own copy of the cached body so that the HTTP layer can
		 * safely pfree(resp->body) after sending the response, without
		 * affecting the cache entry.
		 */
		resp->body = pstrdup(body);
		pfree(body);
		return;
	}

	elog(pgstar_log_level, "pgstar: UI cache MISS for page '%s' (key: %s), rendering from database", page_name, cache_key);

	if (!pgstar_ui_render_page(page_name, &body, &content_type))
	{
		elog(pgstar_log_level, "pgstar: UI render_page returned false for '%s'", page_name);
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"page not found\"}\n");
		return;
	}

	elog(pgstar_log_level, "pgstar: UI render_page succeeded, body=%p, content_type=%p", 
		 (void *)body, (void *)content_type);

	if (body == NULL)
	{
		elog(WARNING, "pgstar: UI render_page returned NULL body for '%s'", page_name);
		resp->status_code = 500;
		resp->status_text = "Internal Server Error";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"internal error: null body\"}\n");
		return;
	}

	bool		content_type_allocated = true; /* Assume allocated by pgstar_ui_render_page */
	
	if (content_type == NULL)
	{
		elog(WARNING, "pgstar: UI render_page returned NULL content_type for '%s'", page_name);
		content_type = pstrdup("text/html; charset=utf-8");
		content_type_allocated = true; /* We allocated it */
	}

	elog(pgstar_log_level, "pgstar: UI cache PUT for page '%s' (key: %s), body_len=%zu, content_type='%s'", 
		 page_name, cache_key, body ? strlen(body) : 0, content_type ? content_type : "(null)");
	
	char *body_cache = pstrdup(body); // this copy is for the cache and freed later.
	// elog(pgstar_log_level, "pgstar: UI cache PUT: body_copy created at %p", (void *)body_cache);

	pgstar_ui_cache_put(cache_key, body_cache, content_type);
	elog(pgstar_log_level, "pgstar: UI cache PUT completed");

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = pstrdup(content_type);
	resp->body = pstrdup(body);

	if (content_type_allocated) {
		pgstar_safe_pfree(content_type);
	}

	/* Note: We don't free 'body' or 'content_type' here because:
	 * 1. They were allocated by pgstar_ui_render_page() in a transaction context
	 * 2. We've already copied them to resp->body and resp->content_type
	 * 3. The transaction memory context will be cleaned up automatically
	 * 4. Attempting to free them here can cause segfaults if the memory context
	 *    has changed or been reset after the SPI operations completed.
	 */

	elog(pgstar_log_level, "pgstar: UI pages route completed successfully");
}

/*
 * Route: /static/js/name.js
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
 * Route: /static/css/name.css
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

	elog(pgstar_log_level, "pgstar: cache_put entry: key=%s, body=%p, content_type=%p", 
		 key ? key : "(null)", (void *)body, (void *)content_type);

	if (pgstar_ui_cache_ttl <= 0)
	{
		elog(pgstar_log_level, "pgstar: cache_put: cache disabled (ttl=%d)", pgstar_ui_cache_ttl);
		return;
	}

	if (key == NULL)
	{
		elog(WARNING, "pgstar: cache_put: NULL key, skipping");
		return;
	}

	if (body == NULL)
	{
		elog(WARNING, "pgstar: cache_put: NULL body for key '%s', skipping", key);
		return;
	}

	if (content_type == NULL)
	{
		elog(WARNING, "pgstar: cache_put: NULL content_type for key '%s', using default", key);
		content_type = "text/html; charset=utf-8";
	}

	now = time(NULL);
	// elog(pgstar_log_level, "pgstar: cache_put: finding slot, now=%ld", (long)now);

	for (i = 0; i < (int) lengthof(pgstar_ui_cache); i++)
	{
		PgstarUiCacheEntry *e = &pgstar_ui_cache[i];

		if (!e->valid)
		{
			slot = i;
			// elog(pgstar_log_level, "pgstar: cache_put: found empty slot %d", i);
			break;
		}

		if (!e->valid || e->expires_at <= now)
		{
			slot = i;
			// elog(pgstar_log_level, "pgstar: cache_put: found expired slot %d", i);
			break;
		}

		if (slot < 0 || e->expires_at < oldest)
		{
			slot = i;
			oldest = e->expires_at;
		}
	}

	if (slot < 0)
	{
		// elog(pgstar_log_level, "pgstar: cache_put: no empty/expired slot, finding LRU slot");
		/* Find oldest entry (LRU eviction) */
		slot = 0;
		oldest = pgstar_ui_cache[0].expires_at;
		for (i = 1; i < (int) lengthof(pgstar_ui_cache); i++)
		{
			PgstarUiCacheEntry *e = &pgstar_ui_cache[i];
			if (e->expires_at < oldest)
			{
				slot = i;
				oldest = e->expires_at;
			}
		}
		// elog(pgstar_log_level, "pgstar: cache_put: selected LRU slot %d (expires_at=%ld)", slot, (long)oldest);
	}

	if (slot < 0)
	{
		elog(WARNING, "pgstar: cache_put: failed to find slot, cache full");
		return;
	}

	// elog(pgstar_log_level, "pgstar: cache_put: using slot %d", slot);

	pgstar_ui_cache[slot].valid = true;
	// elog(pgstar_log_level, "pgstar: cache_put: copying key");
	strlcpy(pgstar_ui_cache[slot].key, key, sizeof(pgstar_ui_cache[slot].key));

	elog(pgstar_log_level, "pgstar: cache_put: freeing old body if exists");
	if (pgstar_ui_cache[slot].body)
	{
		pgstar_safe_pfree(pgstar_ui_cache[slot].body);
	}

	// elog(pgstar_log_level, "pgstar: cache_put: storing body");
	pgstar_ui_cache[slot].body = body;  /* body is already pstrdup'd by caller */

	// elog(pgstar_log_level, "pgstar: cache_put: copying content_type");
	strlcpy(pgstar_ui_cache[slot].content_type, content_type,
			sizeof(pgstar_ui_cache[slot].content_type));

	// elog(pgstar_log_level, "pgstar: cache_put: setting expires_at");
	pgstar_ui_cache[slot].expires_at = now + pgstar_ui_cache_ttl;
	elog(pgstar_log_level, "pgstar: cache_put: completed successfully");
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

			/* Copy buf.data before SPI_finish() and CommitTransactionCommand() 
			 * reset the memory context. The caller will own this copy. */
			*body = pstrdup(buf.data);
			*content_type = pstrdup(ct);
			
			/* Free the StringInfo buffer since we've copied its data */
			pfree(buf.data);
			
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
			bool		ct_allocated = false;

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
				ct_allocated = true;
			}

			initStringInfo(&buf);
			appendStringInfoString(&buf, content);

			*body = pstrdup(buf.data);
			*content_type = pstrdup(ct);
			
			/* Clean up temporary allocations */
			pgstar_safe_pfree(buf.data);
			/* Free ct only if we allocated it ourselves (when it was NULL).
			 * If ct came from SPI_getvalue(), it will be freed by SPI_finish() 
			 * which is called later in this function. */
			if (ct_allocated)
				pgstar_safe_pfree(ct);
			
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

/* HTML helper functions have been moved to separate files:
 * - ui/pages_layout.c: pgstar_ui_build_html_page()
 * - ui/meta_tags.c: pgstar_ui_append_meta_tags()
 * - ui/css_links.c: pgstar_ui_append_css_links()
 * - ui/js_scripts.c: pgstar_ui_append_js_scripts()
 */


