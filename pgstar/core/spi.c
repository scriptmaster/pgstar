#include "postgres.h"

#include "core/spi.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"

void
pgstar_execute_sql(const char *sql)
{
	int			ret;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errmsg("pgstar: SPI_connect failed: %d", ret)));

	ret = SPI_execute(sql, false, 0);
	if (ret < 0)
		ereport(ERROR,
				(errmsg("pgstar: SPI_execute failed (%d) for: %s", ret, sql)));

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

