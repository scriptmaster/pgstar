#include "postgres.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/json.h"

#include "pgstar.h"

char *
pgstar_json_files(void)
{
    int             ret;
    StringInfoData  buf;
    MemoryContext   caller_ctx;
    MemoryContext   old_ctx;
    char           *result_json;

    caller_ctx = CurrentMemoryContext;

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("pgstar: SPI_connect failed: %d", ret)));

    /* Select file metadata. We don't select the actual payload (text/base64 data)
     * to keep the list lightweight. */
    ret = SPI_execute(
        "SELECT file_id, file_name, content_type, is_binary, is_custom, is_download, download_url, file_size, updated_at "
        "FROM pgstar.files "
        "ORDER BY file_id ASC",
        true, 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errmsg("pgstar: files query failed: %d", ret)));

    if (SPI_tuptable == NULL)
        ereport(ERROR,
                (errmsg("pgstar: SPI_tuptable is NULL")));

    initStringInfo(&buf);
    appendStringInfoString(&buf, "{\"rows\":[");

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        HeapTuple   tup;
        TupleDesc   tupdesc;
        char       *raw_file_id;
        char       *raw_file_name;
        char       *raw_content_type;
        char       *raw_is_binary;
        char       *raw_is_custom;
        char       *raw_is_download;
        char       *raw_download_url;
        char       *raw_file_size;
        char       *raw_updated_at;
        char       *file_id;
        char       *file_name;
        char       *content_type;
        char       *is_binary;
        char       *is_custom;
        char       *is_download;
        char       *download_url;
        char       *file_size;
        char       *updated_at;
        
        if (SPI_tuptable->vals == NULL || i >= SPI_tuptable->alloced)
            ereport(ERROR,
                    (errmsg("pgstar: invalid tuple index %llu", (unsigned long long)i)));
        
        tup = SPI_tuptable->vals[i];
        tupdesc = SPI_tuptable->tupdesc;
        
        if (tup == NULL || tupdesc == NULL)
            ereport(ERROR,
                    (errmsg("pgstar: invalid tuple or tuple descriptor")));
        
        raw_file_id = SPI_getvalue(tup, tupdesc, 1);
        raw_file_name = SPI_getvalue(tup, tupdesc, 2);
        raw_content_type = SPI_getvalue(tup, tupdesc, 3);
        raw_is_binary = SPI_getvalue(tup, tupdesc, 4);
        raw_is_custom = SPI_getvalue(tup, tupdesc, 5);
        raw_is_download = SPI_getvalue(tup, tupdesc, 6);
        raw_download_url = SPI_getvalue(tup, tupdesc, 7);
        raw_file_size = SPI_getvalue(tup, tupdesc, 8);
        raw_updated_at = SPI_getvalue(tup, tupdesc, 9);

        /* Copy SPI_getvalue results immediately - they're in transaction context */
        file_id = raw_file_id ? pstrdup(raw_file_id) : NULL;
        file_name = raw_file_name ? pstrdup(raw_file_name) : NULL;
        content_type = raw_content_type ? pstrdup(raw_content_type) : NULL;
        is_binary = raw_is_binary ? pstrdup(raw_is_binary) : NULL;
        is_custom = raw_is_custom ? pstrdup(raw_is_custom) : NULL;
        is_download = raw_is_download ? pstrdup(raw_is_download) : NULL;
        download_url = raw_download_url ? pstrdup(raw_download_url) : NULL;
        file_size = raw_file_size ? pstrdup(raw_file_size) : NULL;
        updated_at = raw_updated_at ? pstrdup(raw_updated_at) : NULL;

        if (i > 0)
            appendStringInfoString(&buf, ",");

        appendStringInfoString(&buf, "{\"file_id\":");
        if (file_id)
            appendStringInfoString(&buf, file_id);
        else
            appendStringInfoString(&buf, "0");
        
        appendStringInfoString(&buf, ",\"file_name\":");
        if (file_name)
            escape_json(&buf, file_name);
        else
            appendStringInfoString(&buf, "null");
        appendStringInfoString(&buf, ",");
        
        appendStringInfoString(&buf, "\"content_type\":");
        if (content_type)
            escape_json(&buf, content_type);
        else
            appendStringInfoString(&buf, "null");
        appendStringInfoString(&buf, ",");
        
        appendStringInfoString(&buf, "\"is_binary\":");
        appendStringInfoString(&buf, (is_binary && is_binary[0] == 't') ? "true" : "false");
        appendStringInfoString(&buf, ",\"is_custom\":");
        appendStringInfoString(&buf, (is_custom && is_custom[0] == 't') ? "true" : "false");
        appendStringInfoString(&buf, ",\"is_download\":");
        appendStringInfoString(&buf, (is_download && is_download[0] == 't') ? "true" : "false");
        
        appendStringInfoString(&buf, ",\"download_url\":");
        if (download_url)
            escape_json(&buf, download_url);
        else
            appendStringInfoString(&buf, "null");
        
        appendStringInfoString(&buf, ",\"file_size\":");
        if (file_size)
            appendStringInfoString(&buf, file_size);
        else
            appendStringInfoString(&buf, "null");
        
        appendStringInfoString(&buf, ",\"updated_at\":");
        if (updated_at)
            escape_json(&buf, updated_at);
        else
            appendStringInfoString(&buf, "null");
        appendStringInfoString(&buf, "}");

        /* Don't free the copied strings - they're in transaction context and will be cleaned up by SPI_finish() */
        /* The pstrdup() copies ensure the pointers are valid until we use them, but we don't need to free them */
    }

    appendStringInfoString(&buf, "],\"columns\":[\"file_id\",\"file_name\",\"content_type\",\"is_binary\",\"is_custom\",\"is_download\",\"download_url\",\"file_size\",\"updated_at\"]}\n");

    /* Copy result into caller's context before we tear down the transaction context */
    old_ctx = MemoryContextSwitchTo(caller_ctx);
    result_json = pstrdup(buf.data);
    MemoryContextSwitchTo(old_ctx);

    /* Don't free buf.data - it will be cleaned up when transaction context is torn down */
    /* pfree(buf.data); */

    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();

    return result_json;
}

