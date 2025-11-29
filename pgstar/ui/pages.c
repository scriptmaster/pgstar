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
	size_t		body_len;		/* length of body */
	char		content_type[64];
	time_t		expires_at;
} PgstarUiCacheEntry;

static PgstarUiCacheEntry pgstar_ui_cache[16];

static bool pgstar_ui_cache_get(const char *key, char **body, size_t *body_len, const char **content_type);
static void pgstar_ui_cache_put(const char *key, const char *body, size_t body_len, const char *content_type);

static bool pgstar_ui_render_page(const char *page_name, const char *title_override, char **body, const char **content_type);
bool pgstar_ui_fetch_file(const char *file_name,
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

	if (pgstar_ui_cache_get(cache_key, &body, NULL, &content_type))
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
		resp->body = pstrdup(body); /* pgstar_ui_cache_get already returns a copy, so this is redundant but safe. */
									/* Actually, pgstar_ui_cache_get allocates *body. 
									   We should just take ownership of it. */
		/* Wait, my implementation of cache_get above allocates a copy. 
		   So body IS a copy. pstrdup(body) makes ANOTHER copy. 
		   The previous code assumed cache_get returned a copy. 
		   My new cache_get returns a copy. 
		   So resp->body = body; is correct. 
		   Let's check previous logic.
		   Previous: *body = pstrdup(e->body); return true;
		   Caller: resp->body = pstrdup(body); pfree(body);
		   So caller was double-copying.
		   Let's fix caller to just use body.
		*/
		resp->body = body;
		/* resp->body_len is not critical for text pages, uses strlen */
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
		size_t blen = body ? strlen(body) : 0;
		// elog(pgstar_log_level, "pgstar: UI cache PUT: body_copy created at %p", (void *)body_cache);

		pgstar_ui_cache_put(cache_key, body_cache, blen, content_type);
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
	if (pgstar_ui_cache_get(cache_key, &body, &body_len, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = body_len;
		return;
	}

	char file_path[256];
	snprintf(file_path, sizeof(file_path), "js/%s", name);

	if (!pgstar_ui_fetch_file(file_path, &body, &body_len, &content_type))
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
	 * so pointers from pgstar_ui_fetch_file() become invalid. */
	char *body_cache;
	char *body_resp;
	
	if (body_len == 0 && body != NULL)
		body_len = strlen(body);

	if (body_len > 0)
	{
		/* Binary data or Text data - copy with known length */
		body_cache = (char *) palloc(body_len + 1);
		memcpy(body_cache, body, body_len);
		body_cache[body_len] = '\0'; /* Ensure null-termination just in case used as string */
		
		body_resp = (char *) palloc(body_len + 1);
		memcpy(body_resp, body, body_len);
		body_resp[body_len] = '\0';
	}
	else
	{
		/* Empty body */
		body_cache = pstrdup("");
		body_resp = pstrdup("");
		body_len = 0;
	}
	
	char *content_type_cache = pstrdup(content_type);
	char *content_type_resp = pstrdup(content_type);
	
	pgstar_ui_cache_put(cache_key, body_cache, body_len, content_type_cache);

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
	if (pgstar_ui_cache_get(cache_key, &body, &body_len, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = body_len;
		return;
	}

	char file_path[256];
	snprintf(file_path, sizeof(file_path), "css/%s", name);

	if (!pgstar_ui_fetch_file(file_path, &body, &body_len, &content_type))
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
	 * so pointers from pgstar_ui_fetch_file() become invalid. */
	char *body_cache;
	char *body_resp;
	
	if (body_len == 0 && body != NULL)
		body_len = strlen(body);

	if (body_len > 0)
	{
		/* Binary data or Text data - copy with known length */
		body_cache = (char *) palloc(body_len + 1);
		memcpy(body_cache, body, body_len);
		body_cache[body_len] = '\0'; /* Ensure null-termination */
		
		body_resp = (char *) palloc(body_len + 1);
		memcpy(body_resp, body, body_len);
		body_resp[body_len] = '\0';
	}
	else
	{
		/* Empty body */
		body_cache = pstrdup("");
		body_resp = pstrdup("");
		body_len = 0;
	}
	
	char *content_type_cache = pstrdup(content_type);
	char *content_type_resp = pstrdup(content_type);
	
	pgstar_ui_cache_put(cache_key, body_cache, body_len, content_type_cache);

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
pgstar_ui_cache_get(const char *key, char **body, size_t *body_len, const char **content_type)
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
			/* Copy body to new buffer to prevent caller modifying cache */
			if (e->body_len > 0)
			{
				*body = (char *) palloc(e->body_len + 1);
				memcpy(*body, e->body, e->body_len);
				(*body)[e->body_len] = '\0'; /* Ensure null termination */
			}
			else
			{
				*body = pstrdup("");
			}
			
			if (body_len) *body_len = e->body_len;
			*content_type = e->content_type;
			return true;
		}
	}

	return false;
}

static void
pgstar_ui_cache_put(const char *key, const char *body, size_t body_len, const char *content_type)
{
	time_t		now;
	int			i;
	int			slot = -1;
	time_t		oldest = 0;

	elog(pgstar_log_level, "pgstar: cache_put entry: key=%s, body_len=%zu, content_type=%p", 
		 key ? key : "(null)", body_len, (void *)content_type);

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

	/* Switch to TopMemoryContext for cache allocations so they survive the request */
	MemoryContext old_ctx = MemoryContextSwitchTo(TopMemoryContext);

	pgstar_ui_cache[slot].valid = true;
	// elog(pgstar_log_level, "pgstar: cache_put: copying key");
	strlcpy(pgstar_ui_cache[slot].key, key, sizeof(pgstar_ui_cache[slot].key));

	elog(pgstar_log_level, "pgstar: cache_put: freeing old body if exists");
	if (pgstar_ui_cache[slot].body)
	{
		pgstar_safe_pfree(pgstar_ui_cache[slot].body);
	}

	// elog(pgstar_log_level, "pgstar: cache_put: storing body");
	/* Allocate new body in TopMemoryContext and copy */
	pgstar_ui_cache[slot].body = (char *) palloc(body_len + 1);
	memcpy(pgstar_ui_cache[slot].body, body, body_len);
	pgstar_ui_cache[slot].body[body_len] = '\0'; /* Ensure null-termination */
	
	pgstar_ui_cache[slot].body_len = body_len;

	// elog(pgstar_log_level, "pgstar: cache_put: copying content_type");
	strlcpy(pgstar_ui_cache[slot].content_type, content_type,
			sizeof(pgstar_ui_cache[slot].content_type));

	// elog(pgstar_log_level, "pgstar: cache_put: setting expires_at");
	pgstar_ui_cache[slot].expires_at = now + pgstar_ui_cache_ttl;
	
	MemoryContextSwitchTo(old_ctx);
	
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
		char *quoted_name = quote_literal_cstr(page_name);
		StringInfoData query_buf;

		initStringInfo(&query_buf);
		
		appendStringInfo(&query_buf, 
			"SELECT parent_tag, html, content_type, meta, js, css "
			"FROM pgstar.pages "
			"WHERE page_name = %s "
			"LIMIT 1",
			quoted_name);

		/* 
		 * Note: using SPI_execute instead of SPI_execute_with_args to use inlined parameters
		 * as per project guidelines. quote_literal_cstr handles escaping.
		 */
		ret = SPI_execute(query_buf.data, true, 1);
		
		/* 
		 * Do NOT free quoted_name or query_buf.data here if SPI_execute fails
		 * or if memory context management is tricky.
		 * Since we are in a short-lived request_context, leaking these small
		 * strings is safer than risking double-free or invalid free crashes.
		 */
		/* pfree(quoted_name); */
		/* pfree(query_buf.data); */

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_execute failed: %d", ret)));

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
			/* pfree(buf.data); -- Let request_context clean this up to avoid potential double-free issues */
			
			found = true;
		}
	}
	PG_CATCH();
	{
		ErrorData *edata = CopyErrorData();
		
		elog(pgstar_log_level, "pgstar: ui render_page query failed for '%s': %s", 
			 page_name, edata ? edata->message : "unknown error");
		
		if (edata)
			FreeErrorData(edata);

		FlushErrorState();
		SPI_finish();
		PopActiveSnapshot();
		AbortCurrentTransaction();
		return false;
	}
	PG_END_TRY();

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return found;
}

bool
pgstar_ui_fetch_file(const char *file_name,
					  char **body, size_t *body_len, const char **content_type)
{
	int			ret;
	bool		found = false;
	MemoryContext oldcontext;

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
		elog(pgstar_log_level, "pgstar: fetching asset (file_name='%s') from pgstar.files", file_name);

		/* Construct query with inlined parameter to debug SPI parameter issues */
		{
			/* Use SPI_execute_with_args for safety */
			Oid argtypes[1];
			Datum values[1];
			StringInfoData query_buf;
			
			argtypes[0] = TEXTOID;
			values[0] = CStringGetTextDatum(file_name);
			
			initStringInfo(&query_buf);
			appendStringInfoString(&query_buf, 
				"SELECT text_data, binary_data, is_binary, content_type, is_download "
				"FROM pgstar.files "
				"WHERE file_name = $1 "
				"ORDER BY is_custom DESC, file_id DESC "
				"LIMIT 1");
			
			elog(pgstar_log_level, "pgstar: executing query with args: %s", query_buf.data);
			
			ret = SPI_execute_with_args(query_buf.data, 1, argtypes, values, NULL, true, 1);
			
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
			bool		is_download = false;
			
			/* IMPORTANT: We must make copies of SPI-managed values (text_data, ct) 
			 * immediately because subsequent SPI calls (like the UPDATE below) 
			 * will invalidate the current SPI_tuptable and free this memory. */

			char *raw_text_data = SPI_getvalue(tup, tupdesc, 1);
			text_data = raw_text_data ? pstrdup(raw_text_data) : NULL;
			
			/* Correctly handle binary data (bytea) which might be TOASTed */
			Datum binary_datum = SPI_getbinval(tup, tupdesc, 2, &isnull);
			if (!isnull)
			{
				/* Detoast if necessary. DatumGetByteaP ensures we get a flat, non-compressed varlena. */
				/* We don't need to copy binary_data immediately unless we use it after UPDATE.
				 * But to be safe, we'll extract it later or rely on the fact we only use it
				 * if we DON'T do the download/update path, or if we re-fetch.
				 * Actually, let's just hold the Datum, but remember we can't touch the Tuple 
				 * after another SPI call.
				 * Wait, DatumGetByteaP returns a pointer to palloc'd memory (if it was detoasted)
				 * OR a pointer into the tuple buffer (if it wasn't).
				 * To be safe, we should assume it might be invalidated. 
				 * However, binary_data is only used in the read path.
				 * If we download, we overwrite binary_data with new data.
				 */
				binary_data = DatumGetByteaP(binary_datum);
			}
			else
			{
				binary_data = NULL;
			}

			if (!isnull && binary_data != NULL)
			{
				Datum is_binary_datum = SPI_getbinval(tup, tupdesc, 3, &isnull);
				is_binary = !isnull && DatumGetBool(is_binary_datum);
			}
			
			char *raw_ct = SPI_getvalue(tup, tupdesc, 4);
			ct = raw_ct ? pstrdup(raw_ct) : NULL;
			
			Datum is_download_datum = SPI_getbinval(tup, tupdesc, 5, &isnull);
			if (!isnull)
				is_download = DatumGetBool(is_download_datum);

			/* Handle download if requested and text_data is a URL */
			if (is_download && text_data != NULL && (strncmp(text_data, "http://", 7) == 0 || strncmp(text_data, "https://", 8) == 0))
			{
				size_t downloaded_len = 0;
				char *downloaded_ct = NULL;
				char *downloaded_body;
				
				elog(pgstar_log_level, "pgstar: attempting download from '%s'", text_data);
				downloaded_body = pgstar_http_download(text_data, &downloaded_len, &downloaded_ct);

				if (downloaded_body != NULL)
				{
					/* Prepare update query */
					char *quoted_fn;
					StringInfoData update_buf;
					Oid argtypes_upd[3];
					Datum values_upd[3];
					char nulls_upd[3] = {' ', ' ', ' '};
					bytea *bdata;
					int upd_ret;

					elog(pgstar_log_level, "pgstar: successfully downloaded '%s' (%zu bytes)", text_data, downloaded_len);
					
					/* Update database with downloaded content */
					/* We need to update the same row we just found. 
					 * Since we don't have the ID handy (didn't select it), we'll update by file_name.
					 * We are in a transaction. */
					 
					/* Use hex encoding for binary data insertion to be safe */
					/* Actually, SPI_execute_with_args is better for binary, but we want to stick to inlined/simple for now if possible?
					 * No, for binary update, parameter binding is much safer and easier than hex escaping.
					 * We already use SPI. Let's use SPI_execute_with_args for the UPDATE.
					 */
					
					quoted_fn = quote_literal_cstr(file_name);
					initStringInfo(&update_buf);
					
					/* Update columns: binary_data, text_data=NULL, is_binary=TRUE, is_download=FALSE, content_type */
					/* Note: We use BYTEA for the content. */
					
					/* We need to use parameters for bytea to avoid escaping hell */
					
					argtypes_upd[0] = BYTEAOID;
					// values_upd[0] set below
					
					argtypes_upd[1] = TEXTOID;
					// values_upd[1] set below
					
					argtypes_upd[2] = TEXTOID; /* file_name */
					values_upd[2] = CStringGetTextDatum(file_name);
					
					/* Create bytea datum */
					/* We must use `byteain()` or direct structure creation, but SPI parameters work best with direct pointer */
					/* However, `PointerGetDatum` might be unsafe if we just cast a char* to bytea*. 
					   The bytea struct has a 4-byte header for size (varlena). */
					
					bdata = (bytea *) palloc(downloaded_len + VARHDRSZ);
					SET_VARSIZE(bdata, downloaded_len + VARHDRSZ);
					memcpy(VARDATA(bdata), downloaded_body, downloaded_len);
					values_upd[0] = PointerGetDatum(bdata);
					
					if (downloaded_ct)
						values_upd[1] = CStringGetTextDatum(downloaded_ct);
					else
						values_upd[1] = CStringGetTextDatum(ct ? ct : "application/octet-stream");

					upd_ret = SPI_execute_with_args(
						"UPDATE pgstar.files SET "
						"binary_data = $1, "
						"text_data = NULL, "
						"is_binary = TRUE, "
						"is_download = FALSE, "
						"content_type = $2, "
						"updated_at = now() "
						"WHERE file_name = $3",
						3, argtypes_upd, values_upd, nulls_upd,
						false, 0);
						
					if (upd_ret == SPI_OK_UPDATE) {
						elog(pgstar_log_level, "pgstar: updated pgstar.files with downloaded content");
					} else {
						elog(WARNING, "pgstar: failed to update pgstar.files after download: %d", upd_ret);
					}
					
					/* Use the downloaded data for response */
					/* Free old pointers if needed (they are from SPI, so no manual free) */
					
					/* Set new values for response construction */
					is_binary = true;
					/* CRITICAL FIX: The `bdata` we created above has a VARHDRSZ (4-byte length header).
					 * If we assign `binary_data = bdata;` directly, later code expects `binary_data` to be a Postgres `bytea*`
					 * from which it extracts data using `VARDATA()` and `VARSIZE()`.
					 * 
					 * However, `pgstar_ui_fetch_file` logic below (lines ~890+) does:
					 *   if (is_binary && binary_data != NULL) {
					 *      source_len = VARSIZE(binary_data) - VARHDRSZ;
					 *      bin_data = VARDATA(binary_data);
					 *      ...
					 *   }
					 * 
					 * So `bdata` (which IS a valid bytea struct with header) IS CORRECT for `binary_data`.
					 * 
					 * BUT! If we are seeing "broken binary data", maybe the download itself included extra headers
					 * or the offset is wrong?
					 * 
					 * Wait, `downloaded_body` from `pgstar_http_download` is a pure raw buffer (char*).
					 * We wrapped it into a `bytea` struct in `bdata`.
					 * 
					 * Let's verify if `bdata` persists correctly in the current memory context?
					 * We are inside `SPI_connect()` block. `palloc` allocates in the current SPI context.
					 * `bdata` is passed to SPI_execute_with_args.
					 * Then we assign `binary_data = bdata`.
					 * Then we proceed to use `binary_data`.
					 * 
					 * Logic looks sound for a fresh download.
					 * 
					 * WHAT IF: The "broken" data is because we are mixing up contexts or the `downloaded_body`
					 * wasn't pure binary? (e.g. transfer-encoding chunked?)
					 * The simple HTTP client in client.c DOES NOT handle Transfer-Encoding: chunked.
					 * If the server sends chunked response, we'll see raw chunks with hex sizes in the binary.
					 * 
					 * `emeraldai.cloud` might be behind Nginx/Cloudflare which often uses chunked encoding.
					 */
					binary_data = bdata; /* This is palloc'd in current context */
					text_data = NULL;
					if (downloaded_ct) ct = downloaded_ct;
					
					/* Note: We don't free downloaded_body yet, we copied it to bdata */
					pfree(downloaded_body);
				}
				else
				{
					elog(WARNING, "pgstar: failed to download '%s'", text_data);
				}
			}

			/* Log if file exists but has no data */
			if (text_data == NULL && (isnull || binary_data == NULL))
			{
				elog(pgstar_log_level, "pgstar: file '%s' found but has NULL text_data and binary_data", file_name);
			}

			if (ct == NULL)
			{
				size_t fn_len = strlen(file_name);
				if (fn_len > 3 && strcmp(file_name + fn_len - 3, ".js") == 0)
					ct = pstrdup("application/javascript");
				else if (fn_len > 4 && strcmp(file_name + fn_len - 4, ".css") == 0)
					ct = pstrdup("text/css; charset=utf-8");
				else
					ct = pstrdup("application/octet-stream");
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
				{
					char *bin_data = VARDATA(binary_data);
					temp_body = (char *) palloc(source_len);
					memcpy(temp_body, bin_data, source_len);
					final_body_len = source_len;
				}
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
		AbortCurrentTransaction();
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



/*
 * Route: /favicon.ico
 * Uses pgstar.files row with file_name 'favicon.ico'.
 */
void
pgstar_route_ui_favicon(const PgstarRequest *req, PgstarResponse *resp)
{
	const char *path = req->path;
	char		cache_key[64];
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

	if (strcmp(path, "/favicon.ico") != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	/* Use specific cache key for favicon */
	snprintf(cache_key, sizeof(cache_key), "favicon.ico");

	size_t body_len = 0;
	/* NOTE: Cache currently treats body as string. Binary data with NULLs will be truncated.
	 * Since favicon.ico is binary, we skip cache for now until cache supports binary length.
	 */
	/*
	if (pgstar_ui_cache_get(cache_key, &body, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = 0; 
		return;
	}
	*/

	if (!pgstar_ui_fetch_file("favicon.ico", &body, &body_len, &content_type))
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"favicon not found\"}\n");
		resp->body_len = 0;
		return;
	}

	resp->status_code = 200;
	resp->status_text = "OK";
	/* Ensure content type is set */
	resp->content_type = content_type ? pstrdup(content_type) : pstrdup("image/x-icon");
	
	/* Copy body to response. 
	 * Body from fetch_file is already in current context (copied out of tx),
	 * but we should own our copy if we were caching.
	 * Since we are not caching, we can just take ownership or copy.
	 * To be safe and consistent with other routes that might eventually cache:
	 */
	if (body_len > 0)
	{
		resp->body = (char *) palloc(body_len);
		memcpy(resp->body, body, body_len);
		resp->body_len = body_len;
	}
	else
	{
		resp->body = pstrdup(body);
		resp->body_len = 0;
	}
	
	/* If we were caching, we would put it here. skipped for binary safety. */
}
