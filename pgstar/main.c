#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "core/worker.h"
#include "pgstar.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
PG_FUNCTION_INFO_V1(pgstar_version);

Datum
pgstar_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("pgstar version %s", PGSTAR_VERSION)));
}

void
_PG_init(void)
{
	BackgroundWorker worker;

	pgstar_define_gucs();

	if (!process_shared_preload_libraries_in_progress)
		return;

	MemSet(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 10;
	snprintf(worker.bgw_name, BGW_MAXLEN, "pgstar listener");
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgstar");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgstar_worker");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}
