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
#include "utils/wait_event.h"
#include <unistd.h>

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

	port = pgstar_port;
	if (port < 1025 || port > 65535)
		port = 5481;

	elog(pgstar_log_level, "pgstar: worker booting on port %d", port);

#if PG_VERSION_NUM >= 110000
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user, 0);
#else
	BackgroundWorkerInitializeConnection(pgstar_database, pgstar_user);
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

	elog(pgstar_log_level, "pgstar: worker shutdown");
	proc_exit(0);
}

