#include "postgres.h"

#include "core/signals.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/proc.h"

volatile sig_atomic_t pgstar_got_sigterm = 0;

static void pgstar_sigterm(SIGNAL_ARGS);

void
pgstar_register_sigterm(void)
{
	pqsignal(SIGTERM, pgstar_sigterm);
	pqsignal(SIGHUP, SIG_IGN);
	BackgroundWorkerUnblockSignals();
}

static void
pgstar_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	pgstar_got_sigterm = 1;

	if (MyLatch)
		SetLatch(MyLatch);

	errno = save_errno;
}

