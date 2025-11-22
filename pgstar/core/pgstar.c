#include "postgres.h"

#include "core/pgstar.h"
#include "pgstar_version.h"

#include "utils/guc.h"

const char *PGSTAR_VERSION = PGSTAR_VERSION_STRING;
int pgstar_port = 5481;
int pgstar_log_level = INFO;
char *pgstar_database = "postgres";
char *pgstar_user = NULL;

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
}

