#ifndef PGSTAR_BUNDLER_H
#define PGSTAR_BUNDLER_H

#include "postgres.h"

/* Bundle entry structure - stored in TopMemoryContext */
typedef struct PgstarBundleEntry
{
	char	   *bundle_name;	/* e.g., "app.js" */
	char	   *content;		/* concatenated file contents */
	size_t		content_len;	/* content length */
	struct PgstarBundleEntry *next;
} PgstarBundleEntry;

/* File mtime cache entry - stored in TopMemoryContext */
typedef struct PgstarFileMtimeEntry
{
	char	   *file_path;		/* full path to file */
	time_t		mtime;			/* last known modification time */
	struct PgstarFileMtimeEntry *next;
} PgstarFileMtimeEntry;

/* Initialize bundles from bundle.json at startup */
void pgstar_bundler_init(void);

/* Check if bundles need to be reloaded (call periodically) */
void pgstar_bundler_check_reload(void);

/* Get a bundle by name (returns NULL if not found) */
const PgstarBundleEntry *pgstar_bundler_get(const char *bundle_name);

/* Route handler for /static/bundle/ requests */
void pgstar_route_bundle(const PgstarRequest *req, PgstarResponse *resp);

#endif /* PGSTAR_BUNDLER_H */

