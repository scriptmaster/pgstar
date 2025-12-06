#include "postgres.h"

#include "pgstar.h"
#include "ui/bundler.h"
#include "core/stat.h"

#include "lib/stringinfo.h"
#include "storage/fd.h"
#include "utils/memutils.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Global bundle cache - stored in TopMemoryContext */
static PgstarBundleEntry *pgstar_bundles = NULL;
static bool pgstar_bundles_initialized = false;
static time_t pgstar_bundle_json_mtime = 0;
static char *pgstar_bundle_json_path = NULL;

/* Forward declarations */
static void pgstar_bundler_load_bundle_json(const char *frontend_path);
static PgstarBundleEntry *pgstar_bundler_parse_bundle_json(const char *json_path, const char *frontend_path);
static char *pgstar_bundler_read_file(const char *filepath);
static char *pgstar_bundler_build_path(const char *base, const char *relative);
static void pgstar_bundler_trim_whitespace(char *str);
static bool pgstar_bundler_parse_json_string(const char **json, char *out, size_t out_len);
static bool pgstar_bundler_parse_json_array(const char **json, StringInfoData *files);
static void pgstar_bundler_clear_cache(void);
static bool pgstar_bundler_check_files_changed(const char *frontend_path, const PgstarBundleEntry *bundles);

/*
 * Initialize bundles at startup - called from worker initialization
 */
void
pgstar_bundler_init(void)
{
	const char *frontend_path;

	if (pgstar_bundles_initialized)
		return;

	/* Get frontend path from GUC, default to /var/lib/postgresql/pgstar/frontend */
	frontend_path = pgstar_frontend_path;
	if (frontend_path == NULL || frontend_path[0] == '\0')
	{
		/* Default absolute path - accessible by postgres user */
		frontend_path = "/var/lib/postgresql/pgstar/frontend";
	}

	elog(pgstar_log_level, "pgstar: initializing bundles from frontend path: %s", frontend_path);

	PG_TRY();
	{
		pgstar_bundler_load_bundle_json(frontend_path);
		
		/* Only mark as initialized if bundles were actually loaded */
		if (pgstar_bundles != NULL)
		{
			pgstar_bundles_initialized = true;
			elog(pgstar_log_level, "pgstar: bundles initialized successfully");
		}
		else
		{
			elog(WARNING, "pgstar: bundle initialization completed but no bundles were loaded");
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		FlushErrorState();

		if (edata)
		{
			elog(WARNING,
				 "pgstar: bundle initialization failed: %s",
				 edata->message);
			FreeErrorData(edata);
		}
		/* Continue without bundles - endpoints will return 404 */
	}
	PG_END_TRY();
}

/*
 * Load bundle.json and parse all bundles
 */
static void
pgstar_bundler_load_bundle_json(const char *frontend_path)
{
	char	   *bundle_json_path;
	PgstarBundleEntry *entry;
	MemoryContext old_ctx;

	/* Build path to bundle.json */
	bundle_json_path = pgstar_bundler_build_path(frontend_path, "bundle.json");

	elog(pgstar_log_level, "pgstar: checking for bundle.json at: %s", bundle_json_path);

	if (!pgstar_file_exists(bundle_json_path))
	{
		elog(WARNING, "pgstar: bundle.json not found at %s, skipping bundle initialization", bundle_json_path);
		elog(WARNING, "pgstar: frontend_path was: %s", frontend_path);
		elog(WARNING, "pgstar: check file permissions - PostgreSQL user must be able to read the file");
		pgstar_safe_pfree(bundle_json_path);
		return;
	}

	elog(pgstar_log_level, "pgstar: loading bundles from %s", bundle_json_path);

	/* Store bundle.json path and mtime for change detection */
	if (pgstar_bundle_json_path != NULL)
		pgstar_safe_pfree(pgstar_bundle_json_path);
	pgstar_bundle_json_path = pstrdup(bundle_json_path);
	pgstar_bundle_json_mtime = pgstar_file_mtime(bundle_json_path);

	/* Switch to TopMemoryContext for bundle storage */
	old_ctx = MemoryContextSwitchTo(TopMemoryContext);

	/* Clear old cache if reloading */
	if (pgstar_bundles != NULL)
		pgstar_bundler_clear_cache();

	/* Parse bundle.json and create bundle entries - pass frontend_path directly */
	entry = pgstar_bundler_parse_bundle_json(bundle_json_path, frontend_path);
	if (entry != NULL)
	{
		int			bundle_count = 0;
		PgstarBundleEntry *e;

		pgstar_bundles = entry;
		
		/* Count and log loaded bundles */
		for (e = entry; e != NULL; e = e->next)
			bundle_count++;
		
		elog(pgstar_log_level, "pgstar: loaded %d bundle(s) successfully", bundle_count);
		for (e = entry; e != NULL; e = e->next)
			elog(pgstar_log_level, "pgstar:   - bundle '%s' (%zu bytes)", e->bundle_name, e->content_len);
	}
	else
	{
		elog(WARNING, "pgstar: failed to parse bundle.json or no bundles found");
	}

	MemoryContextSwitchTo(old_ctx);
	pgstar_safe_pfree(bundle_json_path);
}

/*
 * Parse bundle.json file and create bundle entries
 * Format: { "app.js": ["folder/file1.js", "file2.js"], "other.js": ["file3.js"] }
 */
static PgstarBundleEntry *
pgstar_bundler_parse_bundle_json(const char *json_path, const char *frontend_path)
{
	FILE	   *file;
	char	   *json_content;
	const char *cursor;
	PgstarBundleEntry *head = NULL;
	PgstarBundleEntry **tail = &head;

	/* Read entire bundle.json file */
	json_content = pgstar_bundler_read_file(json_path);
	if (json_content == NULL)
	{
		elog(WARNING, "pgstar: failed to read bundle.json from %s", json_path);
		return NULL;
	}

	elog(DEBUG1, "pgstar: read bundle.json, length: %zu", strlen(json_content));

	elog(pgstar_log_level, "pgstar: frontend path: %s", frontend_path);

	/* Parse JSON manually - simple format: { "key": ["val1", "val2"], ... } */
	cursor = json_content;
	while (*cursor && isspace((unsigned char) *cursor))
		cursor++;

	if (*cursor != '{')
	{
		elog(WARNING, "pgstar: bundle.json must start with '{', found '%c' at position %zu", 
			 *cursor, (size_t)(cursor - json_content));
		pgstar_safe_pfree(json_content);
		return NULL;
	}
	cursor++; /* skip '{' */

	elog(DEBUG1, "pgstar: starting to parse bundle.json");

	while (*cursor)
	{
		char		bundle_name[256];
		StringInfoData file_list;
		char	   *file_token;
		char	   *saveptr = NULL;
		PgstarBundleEntry *entry;
		StringInfoData bundle_content;

		/* Skip whitespace */
		while (*cursor && isspace((unsigned char) *cursor))
			cursor++;

		if (*cursor == '}')
			break; /* end of object */

		if (*cursor == ',')
		{
			cursor++;
			continue;
		}

		/* Parse bundle name (key) - parse_json_string expects cursor at the opening quote */
		if (*cursor != '"')
		{
			elog(WARNING, "pgstar: expected '\"' for bundle name, found '%c' at position %zu", 
				 *cursor, (size_t)(cursor - json_content));
			break;
		}

		if (!pgstar_bundler_parse_json_string(&cursor, bundle_name, sizeof(bundle_name)))
		{
			elog(WARNING, "pgstar: failed to parse bundle name at position %zu", 
				 (size_t)(cursor - json_content));
			break;
		}

		elog(DEBUG1, "pgstar: parsed bundle name: '%s'", bundle_name);

		/* Skip whitespace and ':' */
		while (*cursor && (isspace((unsigned char) *cursor) || *cursor == ':'))
			cursor++;

		/* Parse array of files */
		if (*cursor != '[')
		{
			elog(WARNING, "pgstar: expected '[' for file list in bundle '%s'", bundle_name);
			break;
		}

		initStringInfo(&file_list);
		if (!pgstar_bundler_parse_json_array(&cursor, &file_list))
		{
			elog(WARNING, "pgstar: failed to parse file array for bundle '%s'", bundle_name);
			pgstar_safe_pfree(file_list.data);
			break;
		}

		/* Build bundle content by concatenating all files */
		initStringInfo(&bundle_content);

		/* Parse comma-separated file list */
		file_token = strtok_r(file_list.data, ",", &saveptr);
		while (file_token != NULL)
		{
			char	   *filepath;
			char	   *file_content;
			const char *relative_path;

			pgstar_bundler_trim_whitespace(file_token);

			/* Build full path: frontend_path + "/" + file_path_from_json */
			filepath = pgstar_bundler_build_path(frontend_path, file_token);
			
			elog(DEBUG1, "pgstar: file_token='%s', frontend_path='%s', final_path='%s'", 
				 file_token, frontend_path, filepath);

			elog(DEBUG1, "pgstar: loading component file: %s", filepath);

			/* Read file content */
			file_content = pgstar_bundler_read_file(filepath);
			if (file_content != NULL)
			{
				appendStringInfoString(&bundle_content, file_content);
				appendStringInfoChar(&bundle_content, '\n'); /* add newline between files */
				pgstar_safe_pfree(file_content);
			}
			else
			{
				elog(WARNING, "pgstar: failed to read component file: %s", filepath);
			}

			pgstar_safe_pfree(filepath);
			file_token = strtok_r(NULL, ",", &saveptr);
		}

		pgstar_safe_pfree(file_list.data);

		/* Only create bundle entry if we have content */
		if (bundle_content.len > 0)
		{
			/* Create bundle entry */
			entry = (PgstarBundleEntry *) palloc(sizeof(PgstarBundleEntry));
			entry->bundle_name = pstrdup(bundle_name);
			entry->content = bundle_content.data; /* StringInfo owns the memory */
			entry->content_len = bundle_content.len;
			entry->next = NULL;

			*tail = entry;
			tail = &entry->next;

			elog(pgstar_log_level, "pgstar: created bundle '%s' (%zu bytes)", bundle_name, entry->content_len);
		}
		else
		{
			elog(WARNING, "pgstar: bundle '%s' has no content (all component files missing?), skipping", bundle_name);
			pgstar_safe_pfree(bundle_content.data);
		}

		/* Skip whitespace and closing ']' */
		while (*cursor && (isspace((unsigned char) *cursor) || *cursor == ']'))
			cursor++;
	}

	pgstar_safe_pfree(json_content);

	return head;
}

/*
 * Read entire file into a palloc'd string
 */
static char *
pgstar_bundler_read_file(const char *filepath)
{
	FILE	   *file;
	StringInfoData buf;
	char		chunk[4096];
	size_t		nread;
	char	   *result;
	MemoryContext old_ctx;

	file = AllocateFile(filepath, "rb");
	if (file == NULL)
	{
		elog(DEBUG1, "pgstar: could not open file: %s", filepath);
		return NULL;
	}

	old_ctx = MemoryContextSwitchTo(TopMemoryContext);
	initStringInfo(&buf);

	while ((nread = fread(chunk, 1, sizeof(chunk), file)) > 0)
	{
		appendBinaryStringInfo(&buf, chunk, (int) nread);
	}

	FreeFile(file);

	if (buf.len == 0)
	{
		pgstar_safe_pfree(buf.data);
		MemoryContextSwitchTo(old_ctx);
		return NULL;
	}

	/* Ensure null termination */
	appendStringInfoChar(&buf, '\0');
	buf.len--; /* don't count null terminator in length */

	result = buf.data;
	MemoryContextSwitchTo(old_ctx);

	return result;
}

/*
 * Build a file path from base and relative path
 */
static char *
pgstar_bundler_build_path(const char *base, const char *relative)
{
	StringInfoData path;
	char	   *result;

	initStringInfo(&path);

	if (base != NULL && base[0] != '\0')
	{
		appendStringInfoString(&path, base);
		if (base[strlen(base) - 1] != '/')
			appendStringInfoChar(&path, '/');
	}

	if (relative != NULL && relative[0] != '\0')
	{
		/* Remove leading slash from relative if present */
		const char *rel = relative;
		if (*rel == '/')
			rel++;
		appendStringInfoString(&path, rel);
	}

	result = pstrdup(path.data);
	pgstar_safe_pfree(path.data);

	return result;
}

/*
 * Trim whitespace from string in place
 */
static void
pgstar_bundler_trim_whitespace(char *str)
{
	char	   *start = str;
	char	   *end;

	if (str == NULL || *str == '\0')
		return;

	/* Trim leading whitespace */
	while (*start && isspace((unsigned char) *start))
		start++;

	/* Trim trailing whitespace */
	end = str + strlen(str);
	while (end > start && isspace((unsigned char) *(end - 1)))
		end--;

	*end = '\0';

	/* Move string to start if needed */
	if (start != str)
		memmove(str, start, strlen(start) + 1);
}

/*
 * Parse a JSON string value (simple implementation)
 */
static bool
pgstar_bundler_parse_json_string(const char **json, char *out, size_t out_len)
{
	const char *cursor = *json;
	size_t		len = 0;

	if (*cursor != '"')
		return false;

	cursor++; /* skip opening quote */

	while (*cursor && *cursor != '"' && len < out_len - 1)
	{
		if (*cursor == '\\' && cursor[1] != '\0')
		{
			/* Handle escape sequences */
			cursor++;
			switch (*cursor)
			{
				case 'n':
					out[len++] = '\n';
					break;
				case 't':
					out[len++] = '\t';
					break;
				case 'r':
					out[len++] = '\r';
					break;
				case '\\':
					out[len++] = '\\';
					break;
				case '"':
					out[len++] = '"';
					break;
				default:
					out[len++] = *cursor;
					break;
			}
			cursor++;
		}
		else
		{
			out[len++] = *cursor++;
		}
	}

	if (*cursor != '"')
		return false;

	out[len] = '\0';
	*json = cursor + 1; /* skip closing quote */

	return true;
}

/*
 * Parse a JSON array of strings into a comma-separated list
 * Expects cursor to point at the opening '[' character
 */
static bool
pgstar_bundler_parse_json_array(const char **json, StringInfoData *files)
{
	const char *cursor = *json;
	char		value[512];
	bool		first = true;

	while (*cursor && isspace((unsigned char) *cursor))
		cursor++;

	if (*cursor != '[')
		return false;

	cursor++; /* skip '[' */

	while (*cursor)
	{
		while (*cursor && isspace((unsigned char) *cursor))
			cursor++;

		if (*cursor == ']')
		{
			cursor++;
			break;
		}

		if (*cursor == ',')
		{
			cursor++;
			continue;
		}

		if (*cursor != '"')
			return false;

		if (!pgstar_bundler_parse_json_string(&cursor, value, sizeof(value)))
			return false;

		if (!first)
			appendStringInfoChar(files, ',');
		appendStringInfoString(files, value);
		first = false;

		while (*cursor && isspace((unsigned char) *cursor))
			cursor++;
	}

	*json = cursor;
	return true;
}

/*
 * Get a bundle by name
 */
const PgstarBundleEntry *
pgstar_bundler_get(const char *bundle_name)
{
	PgstarBundleEntry *entry;

	if (!pgstar_bundles_initialized)
	{
		elog(DEBUG1, "pgstar: bundles not initialized, cannot get bundle '%s'", bundle_name ? bundle_name : "(null)");
		return NULL;
	}

	if (bundle_name == NULL)
	{
		elog(DEBUG1, "pgstar: bundle name is NULL");
		return NULL;
	}

	if (pgstar_bundles == NULL)
	{
		elog(DEBUG1, "pgstar: no bundles loaded, cannot get bundle '%s'", bundle_name);
		return NULL;
	}

	for (entry = pgstar_bundles; entry != NULL; entry = entry->next)
	{
		if (strcmp(entry->bundle_name, bundle_name) == 0)
		{
			elog(DEBUG1, "pgstar: found bundle '%s'", bundle_name);
			return entry;
		}
	}

	/* Log available bundles for debugging */
	elog(DEBUG1, "pgstar: bundle '%s' not found. Available bundles:", bundle_name);
	for (entry = pgstar_bundles; entry != NULL; entry = entry->next)
		elog(DEBUG1, "pgstar:   - '%s'", entry->bundle_name);

	return NULL;
}

/*
 * Route handler for /static/bundle/ requests
 */
void
pgstar_route_bundle(const PgstarRequest *req, PgstarResponse *resp)
{
	const char *path = req->path;
	const char *bundle_name;
	const PgstarBundleEntry *entry;
	char	   *content_copy;

	memset(resp, 0, sizeof(*resp));

	/* Extract bundle name from path: /static/bundle/app.js -> app.js */
	if (strncmp(path, "/static/bundle/", 15) != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"invalid bundle path\"}\n");
		return;
	}

	bundle_name = path + 15; /* skip "/static/bundle/" */

	if (bundle_name[0] == '\0')
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"bundle name required\"}\n");
		return;
	}

	/* Get bundle */
	entry = pgstar_bundler_get(bundle_name);
	if (entry == NULL)
	{
		StringInfoData error_msg;
		PgstarBundleEntry *e;

		initStringInfo(&error_msg);
		appendStringInfo(&error_msg, "{\"error\":\"bundle not found\",\"requested\":\"%s\",\"available\":[", bundle_name);
		
		/* List available bundles */
		for (e = pgstar_bundles; e != NULL; e = e->next)
		{
			if (e != pgstar_bundles)
				appendStringInfoChar(&error_msg, ',');
			appendStringInfo(&error_msg, "\"%s\"", e->bundle_name);
		}
		appendStringInfoString(&error_msg, "]}\n");

		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = error_msg.data;
		return;
	}

	/* Copy content to caller's memory context */
	content_copy = palloc(entry->content_len + 1);
	memcpy(content_copy, entry->content, entry->content_len);
	content_copy[entry->content_len] = '\0';

	/* Determine content type based on file extension */
	if (strstr(bundle_name, ".js") != NULL)
		resp->content_type = "application/javascript";
	else if (strstr(bundle_name, ".css") != NULL)
		resp->content_type = "text/css";
	else
		resp->content_type = "text/plain";

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->body = content_copy;
	resp->body_len = entry->content_len;
}

/*
 * Clear bundle cache (frees all bundle entries)
 */
static void
pgstar_bundler_clear_cache(void)
{
	PgstarBundleEntry *entry;
	PgstarBundleEntry *next;

	if (pgstar_bundles == NULL)
		return;

	/* Free all entries - they're in TopMemoryContext */
	for (entry = pgstar_bundles; entry != NULL; entry = next)
	{
		next = entry->next;
		/* Content and bundle_name are in TopMemoryContext, will be freed when context is reset */
		/* For now, we just reset the pointer - TopMemoryContext persists */
	}

	pgstar_bundles = NULL;
}

/*
 * Check if bundle.json or any component files have changed
 * Returns true if reload is needed
 */
static bool
pgstar_bundler_check_files_changed(const char *frontend_path, const PgstarBundleEntry *bundles)
{
	char	   *bundle_json_path;
	time_t		current_json_mtime;
	char	   *json_content;
	const char *cursor;
	char		components_dir[1024];
	StringInfoData components_path;
	StringInfoData file_list;
	char	   *file_token;
	char	   *saveptr = NULL;
	bool		needs_reload = false;

	/* Check bundle.json modification time first */
	bundle_json_path = pgstar_bundler_build_path(frontend_path, "bundle.json");
	current_json_mtime = pgstar_file_mtime(bundle_json_path);

	if (current_json_mtime == 0)
	{
		/* bundle.json doesn't exist - no need to reload */
		pgstar_safe_pfree(bundle_json_path);
		return false;
	}

	if (current_json_mtime != pgstar_bundle_json_mtime)
	{
		elog(DEBUG1, "pgstar: bundle.json changed (old: %ld, new: %ld)", 
			 (long) pgstar_bundle_json_mtime, (long) current_json_mtime);
		pgstar_safe_pfree(bundle_json_path);
		return true;
	}

	/* Build components directory path */
	initStringInfo(&components_path);
	appendStringInfo(&components_path, "%s/components", frontend_path);
	snprintf(components_dir, sizeof(components_dir), "%s", components_path.data);
	pgstar_safe_pfree(components_path.data);

	/* Read bundle.json to get list of all component files */
	json_content = pgstar_bundler_read_file(bundle_json_path);
	pgstar_safe_pfree(bundle_json_path);

	if (json_content == NULL)
		return false;

	/* Parse JSON to extract all component file paths and check their mtimes */
	cursor = json_content;
	while (*cursor && isspace((unsigned char) *cursor))
		cursor++;

	if (*cursor != '{')
	{
		pgstar_safe_pfree(json_content);
		return false;
	}
	cursor++; /* skip '{' */

	/* Parse each bundle and check component files */
	while (*cursor && !needs_reload)
	{
		char		bundle_name[256];
		char	   *filepath;
		time_t		file_mtime;

		/* Skip whitespace */
		while (*cursor && isspace((unsigned char) *cursor))
			cursor++;

		if (*cursor == '}')
			break;

		if (*cursor == ',')
		{
			cursor++;
			continue;
		}

		/* Parse bundle name */
		if (*cursor != '"')
			break;
		cursor++;

		if (!pgstar_bundler_parse_json_string(&cursor, bundle_name, sizeof(bundle_name)))
			break;

		/* Skip whitespace and ':' */
		while (*cursor && (isspace((unsigned char) *cursor) || *cursor == ':'))
			cursor++;

		/* Parse array of files */
		if (*cursor != '[')
			break;
		cursor++;

		initStringInfo(&file_list);
		if (!pgstar_bundler_parse_json_array(&cursor, &file_list))
		{
			pgstar_safe_pfree(file_list.data);
			break;
		}

		/* Check each component file's mtime - if any is newer than bundle.json mtime, reload */
		file_token = strtok_r(file_list.data, ",", &saveptr);
		while (file_token != NULL && !needs_reload)
		{
			pgstar_bundler_trim_whitespace(file_token);

			filepath = pgstar_bundler_build_path(components_dir, file_token);
			file_mtime = pgstar_file_mtime(filepath);

			if (file_mtime > current_json_mtime)
			{
				elog(DEBUG1, "pgstar: component file changed: %s (mtime: %ld > bundle.json: %ld)", 
					 filepath, (long) file_mtime, (long) current_json_mtime);
				needs_reload = true;
			}

			pgstar_safe_pfree(filepath);
			file_token = strtok_r(NULL, ",", &saveptr);
		}

		pgstar_safe_pfree(file_list.data);

		/* Skip whitespace and closing ']' */
		while (*cursor && (isspace((unsigned char) *cursor) || *cursor == ']'))
			cursor++;
	}

	pgstar_safe_pfree(json_content);
	return needs_reload;
}

/*
 * Check if bundles need to be reloaded and reload if necessary
 * Called periodically from worker loop (every minute)
 */
void
pgstar_bundler_check_reload(void)
{
	const char *frontend_path;
	bool		needs_reload = false;

	if (!pgstar_bundles_initialized)
		return;

	/* Get frontend path from GUC */
	frontend_path = pgstar_frontend_path;
	if (frontend_path == NULL || frontend_path[0] == '\0')
		frontend_path = "./pgstar/frontend";

	/* Check if files have changed */
	needs_reload = pgstar_bundler_check_files_changed(frontend_path, pgstar_bundles);

	if (needs_reload)
	{
		elog(pgstar_log_level, "pgstar: reloading bundles due to file changes");

		PG_TRY();
		{
			pgstar_bundler_load_bundle_json(frontend_path);
			elog(pgstar_log_level, "pgstar: bundles reloaded successfully");
		}
		PG_CATCH();
		{
			ErrorData  *edata = CopyErrorData();

			FlushErrorState();

			if (edata)
			{
				elog(WARNING,
					 "pgstar: bundle reload failed: %s",
					 edata->message);
				FreeErrorData(edata);
			}
		}
		PG_END_TRY();
	}
}

