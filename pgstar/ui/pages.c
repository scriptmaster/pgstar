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
#include "utils/varlena.h"
#include "common/base64.h"
#include "core/spi.h"

#include <ctype.h>
#include <string.h>
#include <time.h>
#include <limits.h>

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

typedef struct PgstarAssetSeedUrl
{
	const char *file_name;
	const char *url;
} PgstarAssetSeedUrl;

static const PgstarAssetSeedUrl pgstar_asset_seed_urls[] = {
	/* vue.global.prod.min.js removed - now served directly from CDN in js_scripts.c */
	{"favicon.ico", "https://emeraldai.cloud/favicon.ico"}
};

static const bool pgstar_downloads_enabled = true;

static bool pgstar_ui_cache_get(const char *key, char **body, size_t *body_len, const char **content_type);
static void pgstar_ui_cache_put(const char *key, const char *body, size_t body_len, const char *content_type);

static bool pgstar_ui_render_page(const char *page_name, const char *title_override, char **body, const char **content_type);
bool pgstar_ui_fetch_file(const char *file_name,
								  char **body, size_t *body_len, const char **content_type);
static const char *pgstar_ui_seed_url(const char *file_name);
static void pgstar_ui_mark_redownload(const char *file_name, const char *reason);
static bool pgstar_should_store_as_text(const char *content_type, const char *body, size_t len);
static char *pgstar_base64_encode_buffer(const char *src, size_t len);
static char *pgstar_base64_decode_buffer(const char *src, size_t len, size_t *decoded_len);

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
	else if (strcmp(path, "/app/connections") == 0 || strcmp(path, "/app/connections/") == 0)
	{
		strlcpy(page_name, default_page_name, sizeof(page_name));
		title_override = "PGStar: Multi-tenant Connections";
	}
	else if (strncmp(path, "/app/", 5) == 0)
	{
		/* All /app/ paths use the dashboard page */
		strlcpy(page_name, default_page_name, sizeof(page_name));
		
		/* Build title from path segments */
		pgstar_build_title_from_path(path, title_buf, sizeof(title_buf));
		title_override = title_buf;
	}
	else if (strcmp(path, "/ai") == 0 || strcmp(path, "/ai/") == 0)
	{
		strlcpy(page_name, default_page_name, sizeof(page_name));
		title_override = "PGSTAR: Sher AI";
	}
	else if (strncmp(path, "/ai/", 4) == 0)
	{
		/* All /ai/ paths use the dashboard page */
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
		char	   *body_cache;
		size_t		blen;

		if (body == NULL)
		{
			elog(WARNING, "pgstar: UI render_page returned NULL body for '%s'", page_name);
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"internal error: null body\"}\n");
			return;
		}

		if (content_type == NULL)
		{
			elog(WARNING, "pgstar: UI render_page returned NULL content_type for '%s'", page_name);
			content_type = pstrdup("text/html; charset=utf-8");
		}

		elog(pgstar_log_level, "pgstar: UI cache PUT for page '%s' (key: %s), body_len=%zu, content_type='%s'", 
			 page_name, cache_key, body ? strlen(body) : 0, content_type ? content_type : "(null)");

		body_cache = pstrdup(body); // this copy is for the cache
		blen = body ? strlen(body) : 0;

		pgstar_ui_cache_put(cache_key, body_cache, blen, content_type);
		pgstar_safe_pfree(body_cache);
		elog(pgstar_log_level, "pgstar: UI cache PUT completed");

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = pstrdup(content_type);
		resp->body = pstrdup(body);
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
	size_t		body_len = 0;
	char		file_path[256];
	char	   *body_cache = NULL;
	char	   *body_resp = NULL;
	char	   *content_type_cache = NULL;
	char	   *content_type_resp = NULL;

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

	if (pgstar_ui_cache_get(cache_key, &body, &body_len, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = body_len;
		return;
	}

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
	
	content_type_cache = pstrdup(content_type);
	content_type_resp = pstrdup(content_type);
	
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
	size_t		body_len = 0;
	char		file_path[256];
	char	   *body_cache = NULL;
	char	   *body_resp = NULL;
	char	   *content_type_cache = NULL;
	char	   *content_type_resp = NULL;

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

	if (pgstar_ui_cache_get(cache_key, &body, &body_len, &content_type))
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = content_type;
		resp->body = body;
		resp->body_len = body_len;
		return;
	}

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
	
	content_type_cache = pstrdup(content_type);
	content_type_resp = pstrdup(content_type);
	
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
	MemoryContext old_ctx;

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
	old_ctx = MemoryContextSwitchTo(TopMemoryContext);

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

static const char *
pgstar_ui_seed_url(const char *file_name)
{
	if (file_name == NULL)
		return NULL;

	for (size_t i = 0; i < lengthof(pgstar_asset_seed_urls); i++)
	{
		if (strcmp(pgstar_asset_seed_urls[i].file_name, file_name) == 0)
			return pgstar_asset_seed_urls[i].url;
	}

	return NULL;
}

static void
pgstar_ui_mark_redownload(const char *file_name, const char *reason)
{
	const char *seed_url = pgstar_ui_seed_url(file_name);
	Oid			argtypes[2];
	Datum		values[2];
	char		nulls[2] = {' ', ' '};
	bool		need_cleanup = false;

	if (file_name == NULL)
		return;

	if (seed_url == NULL)
	{
		elog(WARNING, "pgstar: unable to reset '%s' for re-download (no seed URL known)", file_name);
		return;
	}

	PG_TRY();
	{
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		need_cleanup = true;

		if (SPI_connect() != SPI_OK_CONNECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_connect failed during redownload reset")));

		argtypes[0] = TEXTOID; /* download_url */
		argtypes[1] = TEXTOID; /* file_name */
		values[0] = CStringGetTextDatum(seed_url);
		values[1] = CStringGetTextDatum(file_name);

		SPI_execute_with_args(
			"UPDATE pgstar.files "
			"SET text_data = NULL, "
			"    download_url = $1, "
			"    is_binary = FALSE, "
			"    is_download = TRUE, "
			"    updated_at = now() "
			"WHERE file_name = $2",
			2, argtypes, values, nulls, false, 0);

		pgstar_safe_pfree(DatumGetPointer(values[0]));
		pgstar_safe_pfree(DatumGetPointer(values[1]));

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		need_cleanup = false;

		elog(pgstar_log_level,
			 "pgstar: reset '%s' to lazy download mode (reason: %s)",
			 file_name,
			 reason ? reason : "unknown");
	}
	PG_CATCH();
	{
		FlushErrorState();

		if (need_cleanup && IsTransactionState())
		{
			SPI_finish();
			PopActiveSnapshot();
			AbortCurrentTransaction();
		}

		elog(WARNING,
			 "pgstar: failed to reset '%s' for re-download (reason: %s)",
			 file_name,
			 reason ? reason : "unknown");
	}
	PG_END_TRY();
}

static bool
pgstar_should_store_as_text(const char *content_type, const char *body, size_t len)
{
	size_t		i;

	if (content_type)
	{
		if (pg_strncasecmp(content_type, "text/", 5) == 0)
			return true;
		if (pg_strncasecmp(content_type, "application/javascript", 22) == 0)
			return true;
		if (pg_strncasecmp(content_type, "application/json", 16) == 0)
			return true;
		if (pg_strncasecmp(content_type, "application/xml", 15) == 0)
			return true;
		if (strstr(content_type, "charset"))
			return true;
	}

	for (i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) body[i];

		if (c == 0)
			return false;
		if (c < 0x09)
			return false;
		if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
			return false;
	}

	return true;
}

static char *
pgstar_base64_encode_buffer(const char *src, size_t len)
{
	int			enc_len;
	int			actual;
	char	   *dst;

	if (len > INT_MAX)
		ereport(ERROR,
				(errmsg("pgstar: base64 encode length overflow: %zu", len)));

	enc_len = pg_b64_enc_len((int) len);
	dst = (char *) palloc(enc_len + 1);
	actual = pg_b64_encode(src, (int) len, dst, enc_len);
	if (actual < 0)
		ereport(ERROR,
				(errmsg("pgstar: base64 encode failed")));

	dst[actual] = '\0';
	return dst;
}

static char *
pgstar_base64_decode_buffer(const char *src, size_t len, size_t *decoded_len)
{
	int			dec_len;
	int			actual;
	char	   *dst;

	if (len > INT_MAX)
		ereport(ERROR,
				(errmsg("pgstar: base64 decode length overflow: %zu", len)));

	dec_len = pg_b64_dec_len((int) len);
	dst = (char *) palloc(dec_len > 0 ? dec_len : 1);
	actual = pg_b64_decode(src, (int) len, dst, dec_len);
	if (actual < 0)
		ereport(ERROR,
				(errmsg("pgstar: base64 decode failed")));

	if (decoded_len)
		*decoded_len = actual;
	return dst;
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
	MemoryContext caller_ctx = CurrentMemoryContext;
	MemoryContext old_ctx;

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
			char	   *parent_tag_raw;
			char	   *html_fragment_raw;
			char	   *ct_raw;
			char	   *parent_tag = NULL;
			char	   *html_fragment = NULL;
			char	   *ct = NULL;
			Datum		meta_datum;
			bool		isnull;
			Jsonb	   *meta = NULL;
			char	   *js_names_raw;
			char	   *css_names_raw;
			char	   *js_names = NULL;
			char	   *css_names = NULL;

			/* CRITICAL: Copy all SPI_getvalue() results immediately - they're only valid until SPI_finish() */
			parent_tag_raw = SPI_getvalue(tup, tupdesc, 1);
			if (parent_tag_raw != NULL)
				parent_tag = pstrdup(parent_tag_raw);

			html_fragment_raw = SPI_getvalue(tup, tupdesc, 2);
			if (html_fragment_raw != NULL)
				html_fragment = pstrdup(html_fragment_raw);

			ct_raw = SPI_getvalue(tup, tupdesc, 3);
			if (ct_raw != NULL)
				ct = pstrdup(ct_raw);
			else
				ct = pstrdup("text/html; charset=utf-8");

			/* For JSONB, we need to copy the datum properly */
			meta_datum = SPI_getbinval(tup, tupdesc, 4, &isnull);
			if (!isnull)
			{
				/* Copy JSONB datum - DatumGetJsonbP returns a pointer that needs to be copied */
				Jsonb *meta_raw = DatumGetJsonbP(meta_datum);
				/* Create a copy in current memory context */
				meta = (Jsonb *) palloc(VARSIZE(meta_raw));
				memcpy(meta, meta_raw, VARSIZE(meta_raw));
			}

			js_names_raw = SPI_getvalue(tup, tupdesc, 5);
			if (js_names_raw != NULL)
				js_names = pstrdup(js_names_raw);

			css_names_raw = SPI_getvalue(tup, tupdesc, 6);
			if (css_names_raw != NULL)
				css_names = pstrdup(css_names_raw);

			initStringInfo(&buf);
			pgstar_ui_build_html_page(page_name,
									  parent_tag ? parent_tag : "",
									  html_fragment ? html_fragment : "",
									  meta,
									  js_names,
									  css_names,
									  title_override,
									  &buf);

			/* Copy into caller's memory context BEFORE cleaning up transaction */
			old_ctx = MemoryContextSwitchTo(caller_ctx);

			if (buf.data)
				*body = pstrdup(buf.data);
			else
				*body = pstrdup("");

			if (ct)
				*content_type = pstrdup(ct);
			else
				*content_type = pstrdup("text/html; charset=utf-8");

			MemoryContextSwitchTo(old_ctx);
			
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
	bool		success = false;
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

	PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: entry", file_name ? file_name : "(null)");

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
				"SELECT text_data, download_url, is_binary, content_type, is_download "
				"FROM pgstar.files "
				"WHERE file_name = $1 "
				"ORDER BY file_id ASC "
				"LIMIT 1");
			
			elog(pgstar_log_level, "pgstar: executing query with args: %s", query_buf.data);
			
			ret = SPI_execute_with_args(query_buf.data, 1, argtypes, values, NULL, true, 1);
			
			pgstar_safe_pfree(query_buf.data);
		}

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar ui: SPI_execute failed: %d", ret)));

		PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: SPI_processed=%llu",
						   file_name ? file_name : "(null)",
						   (unsigned long long) SPI_processed);

		if (SPI_processed == 0)
		{
			int debug_ret;

			elog(pgstar_log_level, "pgstar: file '%s' not found in pgstar.files table", file_name);

			/* DEBUG: List all files to see what's actually there (re-added for debugging) */
			debug_ret = SPI_execute("SELECT file_name FROM pgstar.files", true, 0);
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
				pgstar_safe_pfree(debug_buf.data);
			}
		}
		else if (SPI_processed == 1)
		{
			HeapTuple	tup = SPI_tuptable->vals[0];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			char	   *text_data;
			char	   *download_url;
			bool		is_binary = false;
			char	   *ct;
			bool		ct_allocated = false;
			bool		isnull;
			bool		is_download = false;
			Datum		is_download_datum;
			Datum		is_binary_datum;
			char	   *raw_ct;
			size_t		final_body_len = 0;
			char	   *temp_body = NULL;
			char	   *temp_ct = NULL;
			
			/* IMPORTANT: We must make copies of SPI-managed values (text_data, ct) 
			 * immediately because subsequent SPI calls (like the UPDATE below) 
			 * will invalidate the current SPI_tuptable and free this memory. */

			char *raw_text_data;
			char *raw_download_url;

			raw_text_data = SPI_getvalue(tup, tupdesc, 1);
			text_data = raw_text_data ? pstrdup(raw_text_data) : NULL;

			raw_download_url = SPI_getvalue(tup, tupdesc, 2);
			download_url = raw_download_url ? pstrdup(raw_download_url) : NULL;

			is_binary_datum = SPI_getbinval(tup, tupdesc, 3, &isnull);
			if (!isnull)
				is_binary = DatumGetBool(is_binary_datum);
			
			raw_ct = SPI_getvalue(tup, tupdesc, 4);
			ct = raw_ct ? pstrdup(raw_ct) : NULL;
			
			is_download_datum = SPI_getbinval(tup, tupdesc, 5, &isnull);
			if (!isnull)
				is_download = DatumGetBool(is_download_datum);

			PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: row found is_binary=%d is_download=%d url=%s",
							   file_name ? file_name : "(null)",
							   is_binary ? 1 : 0,
							   is_download ? 1 : 0,
							   download_url ? download_url : "(null)");

			if (is_download)
			{
				const char *remote_url = download_url ? download_url : text_data;

				if (remote_url == NULL)
					ereport(ERROR,
							(errmsg("pgstar: download requested for '%s' but download_url is NULL",
									file_name ? file_name : "(null)")));

				if (!pgstar_downloads_enabled)
				{
					char	   *placeholder = psprintf("/* download disabled: %s (%s) */\n",
													   file_name ? file_name : "(unknown)",
													   remote_url);

					PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: download disabled, serving placeholder for %s",
									   file_name ? file_name : "(null)",
									   remote_url);

					if (text_data)
						pgstar_safe_pfree(text_data);
					text_data = placeholder;
					is_binary = false;
					is_download = false;
				}
				else
				{
					size_t downloaded_len = 0;
					char *downloaded_ct = NULL;
					char *downloaded_body;
					bool	store_as_text;
					Oid		argtypes_upd[5];
					Datum	values_upd[5];
					char	nulls_upd[5] = {' ', ' ', ' ', ' ', ' '};
					int		upd_ret;

					elog(pgstar_log_level, "pgstar: attempting download from '%s'", remote_url);
					downloaded_body = pgstar_http_download(remote_url, &downloaded_len, &downloaded_ct);

					if (downloaded_body != NULL)
					{
						store_as_text = pgstar_should_store_as_text(downloaded_ct ? downloaded_ct : ct,
																	downloaded_body,
																	downloaded_len);
						if (text_data)
							pgstar_safe_pfree(text_data);
						if (store_as_text)
							text_data = pnstrdup(downloaded_body, downloaded_len);
						else
							text_data = pgstar_base64_encode_buffer(downloaded_body, downloaded_len);

						is_binary = !store_as_text;
						is_download = false;

						if (downloaded_ct)
						{
							if (ct_allocated && ct)
								pgstar_safe_pfree(ct);
							ct = downloaded_ct;
							ct_allocated = true;
						}

						/* Store file_size BEFORE base64 encoding (original downloaded size) */
						argtypes_upd[0] = TEXTOID;
						argtypes_upd[1] = BOOLOID;
						argtypes_upd[2] = TEXTOID;
						argtypes_upd[3] = INT8OID;
						argtypes_upd[4] = TEXTOID;
						values_upd[0] = CStringGetTextDatum(text_data);
						values_upd[1] = BoolGetDatum(is_binary);
						values_upd[2] = CStringGetTextDatum(ct ? ct : "application/octet-stream");
						values_upd[3] = Int64GetDatum((int64) downloaded_len);
						values_upd[4] = CStringGetTextDatum(file_name);
						nulls_upd[0] = ' ';
						nulls_upd[1] = ' ';
						nulls_upd[2] = ' ';
						nulls_upd[3] = ' ';
						nulls_upd[4] = ' ';

						upd_ret = SPI_execute_with_args(
							"UPDATE pgstar.files SET "
							"text_data = $1, "
							"is_binary = $2, "
							"is_download = FALSE, "
							"content_type = $3, "
							"file_size = $4, "
							"updated_at = now() "
							"WHERE file_name = $5",
							5, argtypes_upd, values_upd, nulls_upd,
							false, 0);
						
						if (upd_ret != SPI_OK_UPDATE)
							elog(WARNING, "pgstar: failed to update pgstar.files after download: %d", upd_ret);

						pgstar_safe_pfree(downloaded_body);
					}
					else
					{
						elog(WARNING, "pgstar: failed to download '%s'", remote_url);
					}
				}
			}

			if (text_data == NULL)
			{
				elog(pgstar_log_level, "pgstar: file '%s' has NULL text_data", file_name);
				text_data = pstrdup("");
				is_binary = false;
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

			if (is_binary)
			{
				Size decoded_len = 0;

				temp_body = pgstar_base64_decode_buffer(text_data, strlen(text_data), &decoded_len);
				final_body_len = decoded_len;
			}
			else
			{
				temp_body = pstrdup(text_data);
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
			success = true;
			PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: success (binary=%d, len=%zu)",
							   file_name ? file_name : "(null)",
							   (final_body_len > 0) ? 1 : 0,
							   final_body_len);
		}
	}
	PG_CATCH();
	{
		ErrorData *edata = CopyErrorData();
		char		reason_buf[128] = "";
		
		elog(pgstar_log_level, "pgstar: ui fetch asset query failed for '%s': %s", 
			 file_name, edata ? edata->message : "unknown error");
		
		if (edata && edata->message)
			strlcpy(reason_buf, edata->message, sizeof(reason_buf));

		if (edata)
			FreeErrorData(edata);

		FlushErrorState();
		SPI_finish();
		PopActiveSnapshot();
		AbortCurrentTransaction();

		pgstar_ui_mark_redownload(file_name, reason_buf[0] ? reason_buf : NULL);
	}
	PG_END_TRY();

	if (!success)
	{
		PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: returning failure", file_name ? file_name : "(null)");
		/* CRITICAL: Clean up transaction even when file not found */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	PGSTAR_VERBOSE_LOG("ui_fetch_file[%s]: returning success body_len=%zu",
					   file_name ? file_name : "(null)",
					   body_len ? *body_len : 0);

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
	size_t		body_len = 0;

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
