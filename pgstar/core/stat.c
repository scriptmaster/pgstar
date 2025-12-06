#include "postgres.h"

#include "core/stat.h"

#include <sys/stat.h>

bool
pgstar_file_exists(const char *path)
{
	struct stat st;

	if (path == NULL || path[0] == '\0')
		return false;

	if (stat(path, &st) != 0)
	{
		/* Log error for debugging - but only at DEBUG1 level to avoid spam */
		elog(DEBUG1, "pgstar: stat() failed for '%s'", path);
		return false;
	}

	if (!S_ISREG(st.st_mode))
	{
		elog(DEBUG1, "pgstar: '%s' exists but is not a regular file", path);
		return false;
	}

	return true;
}

/*
 * Get file modification time (mtime) as seconds since epoch
 * Returns 0 if file doesn't exist or error occurs
 */
time_t
pgstar_file_mtime(const char *path)
{
	struct stat st;

	if (path == NULL || path[0] == '\0')
		return 0;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return 0;

	return st.st_mtime;
}

