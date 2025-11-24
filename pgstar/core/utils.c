#include "postgres.h"

#include "pgstar.h"

#include "utils/memutils.h"

/*
 * Safe wrapper for pfree that checks for NULL and handles errors gracefully.
 * This prevents segfaults when trying to free NULL pointers or invalid memory.
 */
void
pgstar_safe_pfree(void *ptr)
{
	/* Check for NULL pointer - safe to ignore */
	if (ptr == NULL)
		return;

	/* Use PG_TRY/PG_CATCH to handle any errors gracefully */
	PG_TRY();
	{
		pfree(ptr);
	}
	PG_CATCH();
	{
		/* Log warning and ignore errors - better than crashing */
		elog(WARNING, "WARN: pgstar_safe_pfree failed to free pointer %p, ignoring error", ptr);
		FlushErrorState();
	}
	PG_END_TRY();
}

