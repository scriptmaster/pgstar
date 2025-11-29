#include "postgres.h"

#include "pgstar.h"
#include "pages_layout.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
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

static bool pgstar_ui_render_page(const char *page_name, const char *title_override, char **body, const char **content_type);
static bool pgstar_ui_fetch_asset(const char *kind, const char *name,
								  char **body, size_t *body_len, const char **content_type);

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
static void pgstar_build_title_from_path(const char *path, char *title_buf, size_t title_buf_size);

void
pgstar_route_ui_pages(const PgstarRequest *req, PgstarResponse *resp)
{
	char		page_name[128];
	char		default_page_name[128];
	const char *path = req->path;
	char	   *body = NULL;
	const char *content_type = NULL;
	char		cache_key[256];
	char		title_buf[256];
	const char *title_override = NULL;

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
	memset(title_buf, 0, sizeof(title_buf));

	strlcpy(default_page_name, "dashboard", sizeof(default_page_name));

	if (strcmp(path, "/app") == 0 || strcmp(path, "/app/") == 0)
	{
		strlcpy(page_name, default_page_name, sizeof(page_name));
		title_override = "App Dashboard - PGSTAR App UI";
	}
	else if (strncmp(path, "/app/", 5) == 0)
	{
		/* All /app/* paths use the dashboard page */
		strlcpy(page_name, default_page_name, sizeof(page_name));
		
		/* Build title from path segments */
		pgstar_build_title_from_path(path, title_buf, sizeof(title_buf));
		title_override = title_buf;
	}
	else
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	/* Include title override in cache key to avoid collisions */
	if (title_override)
		snprintf(cache_key, sizeof(cache_key), "page:%s:title:%s", page_name, title_override);
	else
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

	if (!pgstar_ui_render_page(page_name, title_override, &body, &content_type))
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

	{
		bool		content_type_allocated; /* Assume allocated by pgstar_ui_render_page */
		char	   *body_cache;

		if (body == NULL)
		{
			elog(WARNING, "pgstar: UI render_page returned NULL body for '%s'", page_name);
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"internal error: null body\"}\n");
			return;
		}

		content_type_allocated = true; /* Assume allocated by pgstar_ui_render_page */

		if (content_type == NULL)
		{
			elog(WARNING, "pgstar: UI render_page returned NULL content_type for '%s'", page_name);
			content_type = pstrdup("text/html; charset=utf-8");
			content_type_allocated = true; /* We allocated it */
		}

		elog(pgstar_log_level, "pgstar: UI cache PUT for page '%s' (key: %s), body_len=%zu, content_type='%s'", 
			 page_name, cache_key, body ? strlen(body) : 0, content_type ? content_type : "(null)");

		body_cache = pstrdup(body); // this copy is for the cache and freed later.
		// elog(pgstar_log_level, "pgstar: UI cache PUT: body_copy created at %p", (void *)body_cache);

		pgstar_ui_cache_put(cache_key, body_cache, content_type);
		elog(pgstar_log_level, "pgstar: UI cache PUT completed");

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = pstrdup(content_type);
		resp->body = pstrdup(body);

		if (content_type_allocated) {
			pgstar_safe_pfree((void *) content_type);
		}
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

	size_t body_len = 0;
	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = 0; /* Cache doesn't store binary len, will use strlen */
		return;
	}

	if (!pgstar_ui_fetch_asset("js", name, &body, &body_len, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"asset not found\"}\n");
		resp->body_len = 0;
		return;
	}

	/* CRITICAL: Copy body and content_type from transaction context to longer-lived context
	 * before storing in cache. The transaction context is cleaned up after SPI_finish(),
	 * so pointers from pgstar_ui_fetch_asset() become invalid. */
	char *body_cache;
	char *body_resp;
	if (body_len > 0)
	{
		/* Binary data - copy with known length */
		body_cache = (char *) palloc(body_len);
		memcpy(body_cache, body, body_len);
		body_resp = (char *) palloc(body_len);
		memcpy(body_resp, body, body_len);
	}
	else
	{
		/* Text data - use pstrdup (null-terminated) */
		body_cache = pstrdup(body);
		body_resp = pstrdup(body);
	}
	char *content_type_cache = pstrdup(content_type);
	char *content_type_resp = pstrdup(content_type);
	
	pgstar_ui_cache_put(cache_key, body_cache, content_type_cache);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = content_type_resp;
	resp->body = body_resp;
	resp->body_len = body_len;
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

	size_t body_len = 0;
	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = 0; /* Cache doesn't store binary len, will use strlen */
		return;
	}

	if (!pgstar_ui_fetch_asset("css", name, &body, &body_len, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"asset not found\"}\n");
		resp->body_len = 0;
		return;
	}

	/* CRITICAL: Copy body and content_type from transaction context to longer-lived context
	 * before storing in cache. The transaction context is cleaned up after SPI_finish(),
	 * so pointers from pgstar_ui_fetch_asset() become invalid. */
	char *body_cache;
	char *body_resp;
	if (body_len > 0)
	{
		/* Binary data - copy with known length */
		body_cache = (char *) palloc(body_len);
		memcpy(body_cache, body, body_len);
		body_resp = (char *) palloc(body_len);
		memcpy(body_resp, body, body_len);
	}
	else
	{
		/* Text data - use pstrdup (null-terminated) */
		body_cache = pstrdup(body);
		body_resp = pstrdup(body);
	}
	char *content_type_cache = pstrdup(content_type);
	char *content_type_resp = pstrdup(content_type);
	
	pgstar_ui_cache_put(cache_key, body_cache, content_type_cache);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = content_type_resp;
	resp->body = body_resp;
	resp->body_len = body_len;
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
	pgstar_ui_cache[slot].body = (char *) body;  /* body is already pstrdup'd by caller */

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
pgstar_ui_render_page(const char *page_name, const char *title_override, char **body, const char **content_type)
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
									  title_override,
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
					  char **body, size_t *body_len, const char **content_type)
{
	StringInfoData buf;
	int			ret;
	bool		found = false;
	char		file_name[256];
	MemoryContext oldcontext;

	snprintf(file_name, sizeof(file_name), "%s/%s", kind, name);

	/* Save caller's memory context - we'll allocate return values in it */
	oldcontext = CurrentMemoryContext;

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
		values[0] = CStringGetTextDatum(file_name);

		elog(pgstar_log_level, "pgstar: fetching asset '%s' (file_name='%s') from pgstar.files", name, file_name);

		/* Construct query with inlined parameter to debug SPI parameter issues */
		{
			char *quoted_name = quote_literal_cstr(file_name);
			StringInfoData query_buf;
			initStringInfo(&query_buf);
			appendStringInfo(&query_buf, 
				"SELECT text_data, binary_data, is_binary, content_type "
				"FROM pgstar.files "
				"WHERE file_name = %s "
				"ORDER BY is_custom DESC, file_id DESC "
				"LIMIT 1",
				quoted_name);
			
			elog(pgstar_log_level, "pgstar: executing query: %s", query_buf.data);
			
			ret = SPI_execute(query_buf.data, true, 1);
			
			pfree(quoted_name);
			pfree(query_buf.data);
		}

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_execute failed: %d", ret)));

		if (SPI_processed == 0)
		{
			elog(pgstar_log_level, "pgstar: file '%s' not found in pgstar.files table", file_name);

			/* DEBUG: List all files to see what's actually there (re-added for debugging) */
			int debug_ret = SPI_execute("SELECT file_name FROM pgstar.files", true, 0);
			if (debug_ret == SPI_OK_SELECT) {
				StringInfoData debug_buf;
				initStringInfo(&debug_buf);
				appendStringInfo(&debug_buf, "DEBUG: Available files: ");
				for (uint64 i = 0; i < SPI_processed; i++) {
					HeapTuple t = SPI_tuptable->vals[i];
					TupleDesc td = SPI_tuptable->tupdesc;
					char *fn = SPI_getvalue(t, td, 1);
					appendStringInfo(&debug_buf, "'%s', ", fn ? fn : "NULL");
				}
				elog(pgstar_log_level, "%s", debug_buf.data);
				pfree(debug_buf.data);
			}
		}
		else if (SPI_processed == 1)
		{
			HeapTuple	tup = SPI_tuptable->vals[0];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			char	   *text_data;
			bytea	   *binary_data = NULL;
			bool		is_binary = false;
			char	   *ct;
			bool		ct_allocated = false;
			bool		isnull;

			text_data = SPI_getvalue(tup, tupdesc, 1);
			binary_data = (bytea *) SPI_getbinval(tup, tupdesc, 2, &isnull);
			if (!isnull && binary_data != NULL)
			{
				Datum is_binary_datum = SPI_getbinval(tup, tupdesc, 3, &isnull);
				is_binary = !isnull && DatumGetBool(is_binary_datum);
			}
			ct = SPI_getvalue(tup, tupdesc, 4);

			/* Log if file exists but has no data */
			if (text_data == NULL && (isnull || binary_data == NULL))
			{
				elog(pgstar_log_level, "pgstar: file '%s' found but has NULL text_data and binary_data", file_name);
			}

			if (ct == NULL)
			{
				if (strcmp(kind, "js") == 0)
					ct = pstrdup("application/javascript");
				else
					ct = pstrdup("text/css; charset=utf-8");
				ct_allocated = true;
			}

			/* Extract data lengths and content while still in transaction context.
			 * We must read from SPI pointers before switching contexts. */
			size_t final_body_len = 0;
			size_t source_len = 0;
			char *temp_body = NULL;
			char *temp_ct = NULL;
			
			if (is_binary && binary_data != NULL)
			{
				/* Extract binary data while still in transaction context */
				source_len = VARSIZE(binary_data) - VARHDRSZ;
				char *bin_data = VARDATA(binary_data);
				temp_body = (char *) palloc(source_len);
				memcpy(temp_body, bin_data, source_len);
				final_body_len = source_len;
			}
			else if (text_data != NULL)
			{
				/* Extract text data while still in transaction context */
				temp_body = pstrdup(text_data);
				source_len = strlen(text_data);
				final_body_len = 0; /* Will use strlen in response */
			}
			else
			{
				/* Empty file */
				temp_body = pstrdup("");
				source_len = 0;
				final_body_len = 0;
			}
			
			/* Copy content_type while still in transaction context */
			temp_ct = pstrdup(ct);

			/* Switch to caller's memory context before allocating return values
			 * so they persist after the transaction ends */
			MemoryContextSwitchTo(oldcontext);
			
			/* Allocate in caller's context, copying from temporary transaction context data */
			if (final_body_len > 0)
			{
				/* Binary data */
				*body = (char *) palloc(final_body_len);
				memcpy(*body, temp_body, final_body_len);
				*body_len = final_body_len;
			}
			else
			{
				/* Text data - use pstrdup for null-terminated string */
				*body = pstrdup(temp_body);
				*body_len = 0;
			}
			
			*content_type = pstrdup(temp_ct);
			
			/* Note: temp_body and temp_ct are allocated in the transaction context
			 * and will be automatically freed when SPI_finish() and 
			 * CommitTransactionCommand() clean up the transaction context.
			 * We've already copied their contents to the caller's context above. */
			
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
		ErrorData *edata = CopyErrorData();
		
		elog(pgstar_log_level, "pgstar: ui fetch asset query failed for '%s': %s", 
			 file_name, edata ? edata->message : "unknown error");
		
		if (edata)
			FreeErrorData(edata);

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

/*
 * Build title from path segments.
 * Examples:
 *   /app/          -> "App Dashboard - PGSTAR App UI"
 *   /app/tables    -> "Tables - PGSTAR App UI"
 *   /app/tables/users -> "Tables / Users - PGSTAR App UI"
 */
static void
pgstar_build_title_from_path(const char *path, char *title_buf, size_t title_buf_size)
{
	char		segs_buf[8][64];
	int			nseg;
	int			i;
	int			pos = 0;
	bool		first = true;

	if (path == NULL || title_buf == NULL || title_buf_size == 0)
	{
		if (title_buf && title_buf_size > 0)
			title_buf[0] = '\0';
		return;
	}

	/* Split path into segments */
	nseg = pgstar_split_path_segments(path, segs_buf, 8);

	/* Skip the "app" segment (first segment) */
	if (nseg > 0 && strcmp(segs_buf[0], "app") == 0)
	{
		/* Process remaining segments */
		for (i = 1; i < nseg; i++)
		{
			const char *seg = segs_buf[i];
			size_t		seg_len = strlen(seg);
			char		capitalized[64];
			int			j;

			/* Skip empty segments */
			if (seg_len == 0)
				continue;

			/* Remove .html suffix if present */
			if (seg_len >= 5 && pg_strcasecmp(seg + seg_len - 5, ".html") == 0)
				seg_len -= 5;

			if (seg_len == 0)
				continue;

			/* Capitalize first letter */
			memcpy(capitalized, seg, seg_len);
			capitalized[seg_len] = '\0';
			if (capitalized[0] >= 'a' && capitalized[0] <= 'z')
				capitalized[0] = capitalized[0] - 'a' + 'A';

			/* Add separator if not first segment */
			if (!first)
			{
				if (pos + 3 < (int) title_buf_size)
				{
					memcpy(title_buf + pos, " / ", 3);
					pos += 3;
				}
			}

			/* Add segment */
			if (pos + seg_len < (int) title_buf_size)
			{
				memcpy(title_buf + pos, capitalized, seg_len);
				pos += seg_len;
			}

			first = false;
		}
	}

	/* If no segments after "app", use "App Dashboard" */
	if (first)
	{
		if (pos + 13 < (int) title_buf_size)
		{
			memcpy(title_buf + pos, "App Dashboard", 13);
			pos += 13;
		}
	}

	/* Append " - PGSTAR App UI" */
	if (pos + 18 < (int) title_buf_size)
	{
		int n = snprintf(title_buf + pos, title_buf_size - pos, " - PGSTAR App UI");
		if (n > 0)
			pos += n;
	}
	
	/* Ensure null termination */
	if (pos < (int) title_buf_size)
		title_buf[pos] = '\0';
	else if (title_buf_size > 0)
		title_buf[title_buf_size - 1] = '\0';
}

/* HTML helper functions have been moved to separate files:
 * - ui/pages_layout.c: pgstar_ui_build_html_page()
 * - ui/meta_tags.c: pgstar_ui_append_meta_tags()
 * - ui/css_links.c: pgstar_ui_append_css_links()
 * - ui/js_scripts.c: pgstar_ui_append_js_scripts()
 */


