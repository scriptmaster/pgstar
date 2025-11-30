#include "postgres.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/snapmgr.h"
#include "lib/stringinfo.h"

#include "pgstar.h"

char *
pgstar_json_schemata(void)
{
    int             ret;
    StringInfoData  buf;
    MemoryContext   caller_ctx = CurrentMemoryContext;
    MemoryContext   old_ctx;
    char           *result_json;

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("pgstar: SPI_connect failed: %d", ret)));

    ret = SPI_execute(
        "SELECT schema_name "
        "FROM information_schema.schemata "
        "ORDER BY schema_name",
        true, 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errmsg("pgstar: schemata query failed: %d", ret)));

    initStringInfo(&buf);
    appendStringInfoString(&buf, "[");

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        HeapTuple   tup = SPI_tuptable->vals[i];
        TupleDesc   tupdesc = SPI_tuptable->tupdesc;
        char       *schema_name;

        schema_name = SPI_getvalue(tup, tupdesc, 1);
        if (!schema_name)
            schema_name = "";

        if (i > 0)
            appendStringInfoString(&buf, ",");

        appendStringInfo(&buf, "\"%s\"", schema_name);
    }

    appendStringInfoString(&buf, "]\n");

    old_ctx = MemoryContextSwitchTo(caller_ctx);
    result_json = pstrdup(buf.data);
    MemoryContextSwitchTo(old_ctx);

    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();

    return result_json;
}

char *
pgstar_json_tables(void)
{
    int             ret;
    StringInfoData  buf;
    MemoryContext   caller_ctx = CurrentMemoryContext;
    MemoryContext   old_ctx;
    char           *result_json;

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("pgstar: SPI_connect failed: %d", ret)));

    ret = SPI_execute(
        "SELECT table_schema, table_name "
        "FROM information_schema.tables "
        "WHERE table_type = 'BASE TABLE' "
        "ORDER BY table_schema, table_name",
        true, 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errmsg("pgstar: tables query failed: %d", ret)));

    initStringInfo(&buf);
    appendStringInfoString(&buf, "{\"rows\":[");

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        HeapTuple   tup = SPI_tuptable->vals[i];
        TupleDesc   tupdesc = SPI_tuptable->tupdesc;
        char       *schema_name;
        char       *table_name;

        schema_name = SPI_getvalue(tup, tupdesc, 1);
        table_name  = SPI_getvalue(tup, tupdesc, 2);

        if (!schema_name) schema_name = "";
        if (!table_name)  table_name  = "";

        if (i > 0)
            appendStringInfoString(&buf, ",");

        appendStringInfo(&buf,
                         "{\"table_schema\":\"%s\",\"table_name\":\"%s\"}",
                         schema_name, table_name);
    }

    appendStringInfoString(&buf, "],\"columns\":[\"table_schema\",\"table_name\"]}\n");

    old_ctx = MemoryContextSwitchTo(caller_ctx);
    result_json = pstrdup(buf.data);
    MemoryContextSwitchTo(old_ctx);

    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();

    return result_json;
}
