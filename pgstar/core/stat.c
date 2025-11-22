#include "postgres.h"

#include "core/stat.h"

#include <sys/stat.h>

bool
pgstar_file_exists(const char *path)
{
	struct stat st;

	if (path == NULL || path[0] == '\0')
		return false;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

