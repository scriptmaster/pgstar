#include "postgres.h"
#include "storage/fd.h"

#include "core/env.h"
#include "core/stat.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char pgstar_port[32] = "";
static bool pgstar_env_loaded = false;
static const int PGSTAR_DEFAULT_PORT = 5480;

static void pgstar_trim(char *str);
static void pgstar_load_env(void);

static void
pgstar_trim(char *str)
{
	char   *start = str;
	char   *end;

	while (*start && isspace((unsigned char) *start))
		start++;

	if (start != str)
		memmove(str, start, strlen(start) + 1);

	end = str + strlen(str);
	while (end > str && isspace((unsigned char) *(end - 1)))
		end--;

	*end = '\0';
}

static void
pgstar_load_env(void)
{
	FILE	   *file;
	char		line[256];
	const char *env_path;
	const char *fallback_paths[] = {".env", "/etc/pgstar/.env"};
	int			i;

	if (pgstar_env_loaded)
		return;

	pgstar_env_loaded = true;

	env_path = getenv("PGSTAR_ENV_PATH");
	if (env_path != NULL && env_path[0] != '\0')
	{
		if (!pgstar_file_exists(env_path))
		{
			ereport(DEBUG1,
					(errmsg("pgstar: could not open %s, defaulting PGSTAR_PORT placeholder", env_path)));
			return;
		}

		file = AllocateFile(env_path, "r");
		if (file == NULL)
		{
			ereport(DEBUG1,
					(errmsg("pgstar: could not open %s, defaulting PGSTAR_PORT placeholder", env_path)));
			return;
		}
	}
	else
	{
		file = NULL;
		for (i = 0; i < lengthof(fallback_paths); i++)
		{
			const char *candidate = fallback_paths[i];

			if (!pgstar_file_exists(candidate))
				continue;

			file = AllocateFile(candidate, "r");
			if (file != NULL)
			{
				env_path = candidate;
				break;
			}
		}

		if (file == NULL)
		{
			ereport(DEBUG1,
					(errmsg("pgstar: could not open .env or /etc/pgstar/.env, defaulting PGSTAR_PORT placeholder")));
			return;
		}
	}

	while (fgets(line, sizeof(line), file) != NULL)
	{
		char   *cursor = line;
		char   *equals;

		pgstar_trim(cursor);

		if (*cursor == '\0' || *cursor == '#')
			continue;

		equals = strchr(cursor, '=');
		if (equals == NULL)
			continue;

		*equals = '\0';
		equals++;
		pgstar_trim(cursor);
		pgstar_trim(equals);

		if (strcmp(cursor, "PGSTAR_PORT") == 0 || strcmp(cursor, "PORT") == 0)
		{
			snprintf(pgstar_port, sizeof(pgstar_port), "%s", equals);
			break;
		}
	}

	FreeFile(file);
}

void
pgstar_env_init(void)
{
	pgstar_load_env();
}

int
pgstar_env_port(void)
{
	long		port;
	char	   *endptr;

	if (!pgstar_env_loaded)
		pgstar_load_env();

	if (pgstar_port[0] == '\0')
		return PGSTAR_DEFAULT_PORT;

	errno = 0;
	port = strtol(pgstar_port, &endptr, 10);

	if (errno != 0 || endptr == pgstar_port || *endptr != '\0' ||
		port <= 0 || port > 65535)
		return PGSTAR_DEFAULT_PORT;

	return (int) port;
}

