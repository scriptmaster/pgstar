#include "postgres.h"

#include "pgstar.h"

#include "utils/memutils.h"
#include "utils/hsearch.h"

typedef struct PgstarPtrTrackEntry
{
	void	   *key;
	long		refcount;
	char		label[64];
} PgstarPtrTrackEntry;

static HTAB *pgstar_ptr_track_table = NULL;

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

void
pgstar_ptr_track(const char *label, const void *ptr, ssize_t delta, Size bytes)
{
	HASHCTL		ctl;
	PgstarPtrTrackEntry *entry;
	bool		found;
	void	   *key;

	if (ptr == NULL)
	{
		elog(pgstar_log_level,
			 "pgstar: reftrack label=%s ptr=NULL delta=%zd bytes=%zu",
			 label ? label : "(null)", delta, (Size) bytes);
		return;
	}

	if (pgstar_ptr_track_table == NULL)
	{
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(void *);
		ctl.entrysize = sizeof(PgstarPtrTrackEntry);
		ctl.hcxt = TopMemoryContext;
		pgstar_ptr_track_table = hash_create("pgstar ptr tracker", 256,
											 &ctl, HASH_ELEM | HASH_CONTEXT);
	}

	key = (void *) ptr;
	entry = hash_search(pgstar_ptr_track_table, &key, HASH_ENTER, &found);
	if (!found)
	{
		entry->key = key;
		entry->refcount = 0;
		strlcpy(entry->label, label ? label : "unknown",
				sizeof(entry->label));
	}
	else if (label && label[0] != '\0')
	{
		/* Update label to the most recent description for clarity */
		strlcpy(entry->label, label, sizeof(entry->label));
	}

	entry->refcount += delta;

	elog(pgstar_log_level,
		 "pgstar: reftrack label=%s ptr=%p delta=%zd total=%ld bytes=%zu",
		 entry->label, ptr, delta, entry->refcount, (Size) bytes);

	if (entry->refcount <= 0)
	{
		hash_search(pgstar_ptr_track_table, &key, HASH_REMOVE, NULL);
	}
}

