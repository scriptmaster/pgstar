#include "postgres.h"

#include "core/signals.h"
#include "core/spi.h"
#include "core/worker.h"
#include "pgstar.h"
#include "tcp/socket.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/wait_event.h"
#include <unistd.h>

#include <string.h>
#include "utils/memutils.h"

#ifndef WAIT_EVENT_EXTENSION
#define WAIT_EVENT_EXTENSION 0
#endif

static void pgstar_ensure_jobs_table(void);
static void pgstar_ensure_default_user(void);

void
pgstar_worker(Datum main_arg)
{
	int			sockfd = -1;
	int			port;
	int			rc;

	pgstar_register_sigterm();

	port = pgstar_port;
	if (port < 1025 || port > 65535)
		port = 5481;

	elog(pgstar_log_level, "pgstar: worker booting on port %d", port);
	elog(pgstar_log_level, "pgstar: GUC parameters configured - port=%d, log_level=%d, database='%s', user='%s', ui_cache_ttl=%d",
		pgstar_port, pgstar_log_level, pgstar_database, pgstar_user ? pgstar_user : "(null)", pgstar_ui_cache_ttl);

#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user, 0);
#else
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user);
#endif

	/*
	 * Initialization of extension state (schema, users, jobs tables, UI pages).
	 * Wrap in PG_TRY so that any SQL or SPI ERRORs during boot do not cause
	 * the background worker process to exit; we log and continue instead.
	 */
	PG_TRY();
	{
		pgstar_execute_sql("CREATE EXTENSION IF NOT EXISTS pgcrypto");
		pgstar_execute_sql("CREATE SCHEMA IF NOT EXISTS pgstar");
		pgstar_ensure_default_user();
		pgstar_ensure_jobs_table();
		/* pgstar_ui_ensure_pages(); -- MIGRATED TO SQL: pgstar.pages table creation
		 * and seed data insertion has been moved to pgstar--*.sql files for
		 * synchronous creation during CREATE EXTENSION. The function is now a no-op.
		 * Other important tables (users, jobs, etc.) remain in C code as they
		 * require runtime initialization logic. */
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		FlushErrorState();

		if (edata)
		{
			elog(WARNING,
				 "pgstar: worker initialization failed: %s",
				 edata->message);
			FreeErrorData(edata);
		}
	}
	PG_END_TRY();

	sockfd = pgstar_tcp_open_socket(port);

	while (!pgstar_got_sigterm)
	{
		ResetLatch(MyLatch);

		rc = WaitLatchOrSocket(MyLatch,
							   WL_LATCH_SET |
							   WL_SOCKET_READABLE |
							   WL_TIMEOUT |
							   WL_EXIT_ON_PM_DEATH,
							   sockfd,
							   10000L,
							   WAIT_EVENT_EXTENSION);

		if (rc & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();

		if (pgstar_got_sigterm)
			break;

		if (rc & WL_SOCKET_READABLE)
		{
			/*
			 * Handle connections. We should create a memory context for handling 
			 * requests so that we can reset it after each batch of work to prevent leaks.
			 */
			MemoryContext request_context = AllocSetContextCreate(CurrentMemoryContext,
																"pgstar_request_context",
																ALLOCSET_DEFAULT_SIZES);
			MemoryContext old_context = MemoryContextSwitchTo(request_context);

			pgstar_tcp_accept_loop(sockfd);

			MemoryContextSwitchTo(old_context);
			MemoryContextDelete(request_context);
		}

		if (rc & WL_TIMEOUT)
		{
			static int tick_count = 0;
			tick_count++;
			// if (tick_count++ % 6 == 0) /* Log every minute (6 * 10s) */
			elog(pgstar_log_level, "pgstar: http worker heartbeat (tick %d)", tick_count);
			// else
			// 	elog(DEBUG3, "pgstar: worker heartbeat");
		}
	}

	if (sockfd >= 0)
		close(sockfd);

	elog(pgstar_log_level, "pgstar: worker shutdown");
	proc_exit(0);
}

static void
pgstar_ensure_jobs_table(void)
{
	pgstar_execute_sql("CREATE TABLE IF NOT EXISTS pgstar.jobs ("
					   "job_id            BIGSERIAL PRIMARY KEY,"
					   "job_type          TEXT NOT NULL CHECK (job_type IN ('run_once','cron','continuous')),"
					   "cron_expression   TEXT,"
					   "table_name        TEXT NOT NULL,"
					   "key_column        TEXT NOT NULL,"
					   "given_stmt        TEXT NOT NULL,"
					   "parameters        JSONB NOT NULL DEFAULT '{}'::jsonb,"
					   "range_direction   TEXT NOT NULL DEFAULT 'asc' CHECK (range_direction IN ('asc','desc')),"
					   "start_range       NUMERIC NOT NULL,"
					   "end_range         NUMERIC,"
					   "last_record       NUMERIC,"
					   "batch_count       INTEGER NOT NULL DEFAULT 1000,"
					   "start_time        TIMESTAMPTZ NOT NULL DEFAULT now(),"
					   "duration          INTERVAL,"
					   "last_updated_time TIMESTAMPTZ,"
					   "completed_time    TIMESTAMPTZ,"
					   "records_processed BIGINT NOT NULL DEFAULT 0"
					   ")");
	pgstar_execute_sql("CREATE TABLE IF NOT EXISTS pgstar.job_runs ("
					   "job_run_id        BIGSERIAL PRIMARY KEY,"
					   "job_id            BIGINT NOT NULL REFERENCES pgstar.jobs(job_id) ON DELETE CASCADE,"
					   "table_name        TEXT,"
					   "key_column        TEXT,"
					   "start_time        TIMESTAMPTZ NOT NULL DEFAULT now(),"
					   "start_range_number NUMERIC,"
					   "start_batch       INTEGER,"
					   "previous_completion TIMESTAMPTZ,"
					   "records_retrieved BIGINT,"
					   "records_processed BIGINT,"
					   "processed_time    INTERVAL,"
					   "executed_stmt     TEXT,"
					   "parameters        JSONB NOT NULL DEFAULT '{}'::jsonb"
					   ")");
}

/*
 * Ensure there is at least one default pgstarNN@hostname user with a
 * temporary password. The password is a random UUID; we store:
 *  - password_hash  : SHA256(pw) hex via pgcrypto.digest
 *  - temp_pass_b64  : base64(pw) for short-term retrieval
 *  - temp_expires_at: now() + 1 day
 *
 * Username format: pgstarNN@<hostname>, where NN is 10-99 derived from
 * the current backend PID to keep things simple and deterministic per host.
 */
static void
pgstar_ensure_default_user(void)
{
	char		hostname[256];
	int			nn;
	char	   *username;
	char	   *sql;

	if (gethostname(hostname, sizeof(hostname)) != 0)
		strlcpy(hostname, "localhost", sizeof(hostname));
	hostname[sizeof(hostname) - 1] = '\0';

	/* Pick a simple 2-digit suffix 10-99 based on PID */
	nn = (Abs(MyProcPid) % 90) + 10;

	username = psprintf("pgstar%02d@%s", nn, hostname);

	/* Users table for auth */
	pgstar_execute_sql(
		"CREATE TABLE IF NOT EXISTS pgstar.users ("
		"user_id         BIGSERIAL PRIMARY KEY,"
		"username        TEXT NOT NULL UNIQUE,"
		"password_hash   TEXT NOT NULL,"
		"temp_pass_b64   TEXT,"
		"temp_expires_at TIMESTAMPTZ,"
		"created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),"
		"created_by      TEXT,"
		"audit           JSONB NOT NULL DEFAULT '{}'::jsonb"
		")");

	/*
	 * Seed a default user only if it doesn't already exist. The temp password
	 * is a random UUID; we hash it with sha256 and store both hash and
	 * base64-encoded original for temporary use.
	 */
	sql = psprintf(
		"WITH u AS (SELECT gen_random_uuid()::text AS pw) "
		"INSERT INTO pgstar.users ("
		"  username, password_hash, temp_pass_b64, temp_expires_at, "
		"  created_by, audit"
		") "
		"SELECT "
		"  %1$s, "
		"  encode(digest(u.pw, 'sha256'), 'hex'), "
		"  encode(convert_to(u.pw, 'UTF8'), 'base64'), "
		"  now() + interval '1 day', "
		"  'pgstar_worker', "
		"  jsonb_build_object('note','auto-created default user') "
		"FROM u "
		"WHERE NOT EXISTS ("
		"  SELECT 1 FROM pgstar.users LIMIT 1"
		");",
		quote_literal_cstr(username));

	pgstar_execute_sql(sql);

	pfree(sql);
	pfree(username);
}

