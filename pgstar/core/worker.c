#include "postgres.h"

#include "core/env.h"
#include "core/signals.h"
#include "core/spi.h"
#include "core/worker.h"
#include "tcp/socket.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/wait_event.h"

#ifndef WAIT_EVENT_EXTENSION
#define WAIT_EVENT_EXTENSION 0
#endif

void
pgstar_worker(Datum main_arg)
{
	int			sockfd = -1;
	int			port;
	int			rc;

	pgstar_register_sigterm();

	pgstar_env_init();
	port = pgstar_env_port();

	elog(LOG, "pgstar: worker booting on port %d", port);

#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection("postgres", "postgres", 0);
#else
	BackgroundWorkerInitializeConnection("postgres", "postgres");
#endif

	pgstar_execute_sql("CREATE SCHEMA IF NOT EXISTS pgstar");

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
			pgstar_tcp_accept_loop(sockfd);

		if (rc & WL_TIMEOUT)
			elog(DEBUG3, "pgstar: worker heartbeat");
	}

	if (sockfd >= 0)
		close(sockfd);

	elog(LOG, "pgstar: worker shutdown");
	proc_exit(0);
}

