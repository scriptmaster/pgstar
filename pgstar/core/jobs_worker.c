#include "postgres.h"

#include "core/jobs_worker.h"
#include "core/signals.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"

#include "pgstar.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/wait_event.h"

#ifndef WAIT_EVENT_EXTENSION
#define WAIT_EVENT_EXTENSION 0
#endif

static bool pgstar_jobs_tick(void);

void
pgstar_jobs_worker(Datum main_arg)
{
	int			rc;

	pgstar_register_sigterm();

	elog(pgstar_log_level, "pgstar: jobs worker starting");

#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user, 0);
#else
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user);
#endif

	while (!pgstar_got_sigterm)
	{
		bool		did_work = false;

		ResetLatch(MyLatch);

		/*
		 * Execute at most one job per tick. If we did work, use a short
		 * timeout so we can continue quickly; otherwise back off a bit.
		 * Protect the tick with PG_TRY so that any ERROR in job SQL or SPI
		 * does not terminate the background worker; instead we log and
		 * continue the loop.
		 */
		PG_TRY();
		{
			did_work = pgstar_jobs_tick();
		}
		PG_CATCH();
		{
			ErrorData  *edata = CopyErrorData();

			FlushErrorState();

			if (edata)
			{
				elog(WARNING,
					 "pgstar: jobs worker tick failed: %s",
					 edata->message);
				FreeErrorData(edata);
			}

			did_work = false;
		}
		PG_END_TRY();

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET |
					   WL_TIMEOUT |
					   WL_EXIT_ON_PM_DEATH,
					   did_work ? 100L : 1000L,
					   WAIT_EVENT_EXTENSION);

		if (rc & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();

		if (pgstar_got_sigterm)
			break;
	}

	elog(pgstar_log_level, "pgstar: jobs worker shutdown");
	proc_exit(0);
}

/*
 * pgstar_jobs_tick
 *
 * Minimal execution engine:
 *  - Picks one pending run_once job (completed_time IS NULL).
 *  - Executes given_stmt once with start_range / end_range as $1/$2.
 *  - Inserts a pgstar.job_runs row summarising the execution.
 *  - Updates pgstar.jobs.records_processed and timestamps.
 *
 * For now we intentionally do NOT try to manage batch windows or last_record;
 * callers should encode any LIMIT/OFFSET or key paging logic directly in
 * given_stmt, and can still build more advanced orchestration on top of the
 * metadata.
 */
static bool
pgstar_jobs_tick(void)
{
	int			ret;
	bool		did_work = false;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errmsg("pgstar jobs worker: SPI_connect failed: %d", ret)));

	/*
	 * Select a single pending run_once job. For now we avoid FOR UPDATE /
	 * SKIP LOCKED to keep things simple and to ensure compatibility with
	 * all caller contexts.
	 */
	ret = SPI_execute(
		"SELECT job_id, table_name, key_column, given_stmt, parameters, "
		"       start_range, end_range, batch_count, records_processed, "
		"       completed_time "
		"FROM pgstar.jobs "
		"WHERE job_type = 'run_once' "
		"  AND completed_time IS NULL "
		"ORDER BY start_time "
		"LIMIT 1",
		true,
		1);

	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
				(errmsg("pgstar jobs worker: failed to select jobs (code %d)", ret)));

	if (SPI_processed == 0)
		goto out;

	/* We have one job to execute. */
	{
		HeapTuple	tup = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		bool		isnull;
		int64		job_id;
		text	   *table_name_text;
		text	   *key_column_text;
		text	   *given_stmt_text;
		Datum		params_datum;
		bool		params_isnull;
		Datum		start_range;
		Datum		end_range;
		bool		end_range_isnull;
		int32		batch_count;
		int64		prev_records_processed;
		Datum		prev_completed_time;
		bool		prev_completed_isnull;
		char	   *table_name;
		char	   *key_column;
		char	   *given_stmt;
		Datum		values[2];
		char		nulls[2];
		Oid			argtypes[2];
		uint64		rows;

		job_id = DatumGetInt64(SPI_getbinval(tup, tupdesc, 1, &isnull));

		table_name_text = DatumGetTextPP(SPI_getbinval(tup, tupdesc, 2, &isnull));
		table_name = text_to_cstring(table_name_text);

		key_column_text = DatumGetTextPP(SPI_getbinval(tup, tupdesc, 3, &isnull));
		key_column = text_to_cstring(key_column_text);

		given_stmt_text = DatumGetTextPP(SPI_getbinval(tup, tupdesc, 4, &isnull));
		given_stmt = text_to_cstring(given_stmt_text);

		params_datum = SPI_getbinval(tup, tupdesc, 5, &params_isnull);

		start_range = SPI_getbinval(tup, tupdesc, 6, &isnull);
		if (isnull)
			ereport(ERROR,
					(errmsg("pgstar jobs worker: start_range must not be NULL for job_id " INT64_FORMAT,
							(int64) job_id)));

		end_range = SPI_getbinval(tup, tupdesc, 7, &end_range_isnull);

		batch_count = DatumGetInt32(SPI_getbinval(tup, tupdesc, 8, &isnull));
		if (isnull || batch_count <= 0)
			batch_count = 1000;

		prev_records_processed = DatumGetInt64(SPI_getbinval(tup, tupdesc, 9, &isnull));
		if (isnull)
			prev_records_processed = 0;

		prev_completed_time = SPI_getbinval(tup, tupdesc, 10, &prev_completed_isnull);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));
		MemSet(argtypes, 0, sizeof(argtypes));

		argtypes[0] = NUMERICOID;
		values[0] = start_range;

		argtypes[1] = NUMERICOID;
		if (end_range_isnull)
			nulls[1] = true;
		else
			values[1] = end_range;

		/* Execute the job's statement once for the configured range. */
		ret = SPI_execute_with_args(
			given_stmt,
			2,
			argtypes,
			values,
			nulls,
			false,
			batch_count);

		if (ret < 0)
			ereport(ERROR,
					(errmsg("pgstar jobs worker: SPI_execute_with_args failed (%d) for job_id " INT64_FORMAT,
							ret, (int64) job_id)));

		rows = SPI_processed;

		/* Record a job_runs row summarising this execution. */
		{
			Datum		ir_values[10];
			char		ir_nulls[10];
			Oid			ir_types[10];

			MemSet(ir_values, 0, sizeof(ir_values));
			MemSet(ir_nulls, 0, sizeof(ir_nulls));
			MemSet(ir_types, 0, sizeof(ir_types));

			ir_types[0] = INT8OID;		/* job_id */
			ir_values[0] = Int64GetDatum(job_id);

			ir_types[1] = TEXTOID;		/* table_name */
			ir_values[1] = CStringGetTextDatum(table_name);

			ir_types[2] = TEXTOID;		/* key_column */
			ir_values[2] = CStringGetTextDatum(key_column);

			ir_types[3] = NUMERICOID;	/* start_range_number */
			ir_values[3] = start_range;

			ir_types[4] = INT4OID;		/* start_batch (we treat this worker as batch 1) */
			ir_values[4] = Int32GetDatum(1);

			ir_types[5] = TIMESTAMPTZOID;	/* previous_completion */
			if (prev_completed_isnull)
				ir_nulls[5] = true;
			else
				ir_values[5] = prev_completed_time;

			ir_types[6] = INT8OID;		/* records_retrieved */
			ir_values[6] = Int64GetDatum((int64) rows);

			ir_types[7] = INT8OID;		/* records_processed */
			ir_values[7] = Int64GetDatum((int64) rows);

			ir_types[8] = TEXTOID;		/* executed_stmt */
			ir_values[8] = CStringGetTextDatum(given_stmt);

			ir_types[9] = JSONBOID;		/* parameters */
			if (params_isnull)
				ir_nulls[9] = true;
			else
				ir_values[9] = params_datum;

			ret = SPI_execute_with_args(
				"INSERT INTO pgstar.job_runs ("
				"  job_id, table_name, key_column, start_range_number, start_batch,"
				"  previous_completion, records_retrieved, records_processed,"
				"  executed_stmt, parameters"
				") VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
				10,
				ir_types,
				ir_values,
				ir_nulls,
				false,
				0);

			if (ret != SPI_OK_INSERT)
				ereport(ERROR,
						(errmsg("pgstar jobs worker: failed to insert job_runs for job_id " INT64_FORMAT,
								(int64) job_id)));
		}

		/* Update basic progress metadata on the job itself. */
		{
			Datum		up_values[2];
			char		up_nulls[2];
			Oid			up_types[2];

			MemSet(up_values, 0, sizeof(up_values));
			MemSet(up_nulls, 0, sizeof(up_nulls));
			MemSet(up_types, 0, sizeof(up_types));

			up_types[0] = INT8OID;		/* rows this run */
			up_values[0] = Int64GetDatum((int64) rows);

			up_types[1] = INT8OID;		/* job_id */
			up_values[1] = Int64GetDatum(job_id);

			ret = SPI_execute_with_args(
				"UPDATE pgstar.jobs "
				"SET records_processed = COALESCE(records_processed, 0) + $1, "
				"    last_updated_time = now(), "
				"    completed_time = now() "
				"WHERE job_id = $2",
				2,
				up_types,
				up_values,
				up_nulls,
				false,
				0);

			if (ret != SPI_OK_UPDATE)
				ereport(ERROR,
						(errmsg("pgstar jobs worker: failed to update job " INT64_FORMAT,
								(int64) job_id)));
		}

		elog(pgstar_log_level,
			 "pgstar: jobs worker executed job_id " INT64_FORMAT
			 " table=%s key_column=%s rows=" UINT64_FORMAT,
			 (int64) job_id,
			 table_name,
			 key_column,
			 (uint64) rows);

		did_work = true;
	}

out:
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	return did_work;
}


