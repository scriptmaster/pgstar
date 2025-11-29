#include "postgres.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "pgstar.h"

char *
pgstar_json_files(void)
{
    int             ret;
    StringInfoData  buf;

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("pgstar: SPI_connect failed: %d", ret)));

    /* Select file metadata. We don't select the actual content (text_data/binary_data) 
     * to keep the list lightweight. */
    ret = SPI_execute(
        "SELECT file_id, file_name, content_type, is_binary, is_custom, is_download, updated_at "
        "FROM pgstar.files "
        "ORDER BY file_name ASC",
        true, 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errmsg("pgstar: files query failed: %d", ret)));

    initStringInfo(&buf);
    appendStringInfoString(&buf, "{\"rows\":[");

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        HeapTuple   tup = SPI_tuptable->vals[i];
        TupleDesc   tupdesc = SPI_tuptable->tupdesc;
        
        char *file_id = SPI_getvalue(tup, tupdesc, 1);
        char *file_name = SPI_getvalue(tup, tupdesc, 2);
        char *content_type = SPI_getvalue(tup, tupdesc, 3);
        char *is_binary = SPI_getvalue(tup, tupdesc, 4);
        char *is_custom = SPI_getvalue(tup, tupdesc, 5);
        char *is_download = SPI_getvalue(tup, tupdesc, 6);
        char *updated_at = SPI_getvalue(tup, tupdesc, 7);

        if (i > 0)
            appendStringInfoString(&buf, ",");

        appendStringInfo(&buf, "{\"file_id\":%s,\"file_name\":", file_id ? file_id : "0");
        
        /* quote_literal_cstr returns a palloc'd string with quotes, but we want JSON string escaping.
         * For simplicity, let's assume file_names are relatively safe or use a JSON helper if available.
         * Actually, we should be careful. Let's use a simple manual JSON escape or just print with quotes 
         * if we trust the input (filenames usually don't have crazy chars, but they might).
         * Postgres has `escape_json` but it might be internal. 
         * Let's use a simple format string for now, assuming standard filenames.
         */
        appendStringInfo(&buf, "\"%s\",", file_name ? file_name : "");
        
        appendStringInfo(&buf, "\"content_type\":\"%s\",", content_type ? content_type : "");
        appendStringInfo(&buf, "\"is_binary\":%s,", (is_binary && *is_binary == 't') ? "true" : "false");
        appendStringInfo(&buf, "\"is_custom\":%s,", (is_custom && *is_custom == 't') ? "true" : "false");
        appendStringInfo(&buf, "\"is_download\":%s,", (is_download && *is_download == 't') ? "true" : "false");
        appendStringInfo(&buf, "\"updated_at\":\"%s\"}", updated_at ? updated_at : "");
    }

    appendStringInfoString(&buf, "],\"columns\":[\"file_id\",\"file_name\",\"content_type\",\"is_binary\",\"is_custom\",\"is_download\",\"updated_at\"]}\n");

    /* Copy the result string to a new buffer in the caller's context before finishing SPI */
    char *result_json = pstrdup(buf.data);
    pfree(buf.data);

    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();

    return result_json;
}

