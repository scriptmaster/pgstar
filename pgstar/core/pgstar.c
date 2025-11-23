#include "postgres.h"

#include "pgstar.h"
#include "pgstar_version.h"

#include "utils/guc.h"

const char *PGSTAR_VERSION = PGSTAR_VERSION_STRING;
int pgstar_port = 5481;
int pgstar_log_level = INFO;
char *pgstar_database = "postgres";
char *pgstar_user = NULL;
int pgstar_ui_cache_ttl = 5;

static const struct config_enum_entry pgstar_log_level_options[] = {
	{"error", ERROR, false},
	{"warning", WARNING, false},
	{"info", INFO, false},
	{"debug1", DEBUG1, false},
	{"debug2", DEBUG2, false},
	{NULL, 0, false}
};

void
pgstar_define_gucs(void)
{
	DefineCustomIntVariable(
		"pgstar.port",
		"Port for pgstar background worker HTTP server.",
		"Must be between 1025 and 65535.",
		&pgstar_port,
		5481,
		1025,
		65535,
		PGC_SUSET,
		GUC_SUPERUSER_ONLY,
		NULL,
		NULL,
		NULL);

	DefineCustomEnumVariable(
		"pgstar.log_level",
		"Log level for pgstar background worker.",
		"Controls verbosity of pgstar elog() calls.",
		&pgstar_log_level,
		INFO,
		pgstar_log_level_options,
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomStringVariable(
		"pgstar.database",
		"Database name used by the pgstar background worker.",
		NULL,
		&pgstar_database,
		"postgres",
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomStringVariable(
		"pgstar.user",
		"User name used by the pgstar background worker.",
		NULL,
		&pgstar_user,
		NULL,
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomIntVariable(
		"pgstar.ui_cache_ttl",
		"TTL in seconds for pgstar UI HTML/JS/CSS responses.",
		"Set to 0 to disable caching.",
		&pgstar_ui_cache_ttl,
		5,
		0,
		3600,
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL);
}

