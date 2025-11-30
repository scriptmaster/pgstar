#include "postgres.h"

#include "pgstar.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

#include <string.h>

static void pgstar_api_information_schema(int nseg, const char *segs[],
										  PgstarResponse *resp);
static void pgstar_api_generic_table(const PgstarRequest *req,
									 int nseg, const char *segs[],
									 PgstarResponse *resp);
static void pgstar_api_exec(const PgstarRequest *req, PgstarResponse *resp);
static void pgstar_api_build_select_list(const char *query, StringInfo select_list);

void
pgstar_route_api(const PgstarRequest *req, PgstarResponse *resp)
{
	char		segs_buf[4][64];
	const char *segs[4];
	int			nseg;
	int			i;

	nseg = pgstar_split_path_segments(req->path, segs_buf, 4);
	for (i = 0; i < nseg; i++)
		segs[i] = segs_buf[i];

	/* Expect at least /api/... */
	if (nseg < 1 || strcmp(segs[0], "api") != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"invalid api path\"}\n");
		return;
	}

	/* /api/exec special endpoint for arbitrary SQL/SP execution */
	if (nseg == 2 && strcmp(segs[1], "exec") == 0)
	{
		pgstar_api_exec(req, resp);
		return;
	}

	/* Special-case information_schema routes */
	if (nseg >= 2 && strcmp(segs[1], "information_schema") == 0)
	{
		pgstar_api_information_schema(nseg, segs, resp);
		return;
	}

	/* /api/files endpoint */
	if (nseg == 2 && strcmp(segs[1], "files") == 0)
	{
		char *json = NULL;
		PG_TRY();
		{
			json = pgstar_json_files();
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			FlushErrorState();
			
			/* Cleanup transaction */
			if (IsTransactionState()) {
				SPI_finish();
				PopActiveSnapshot();
				AbortCurrentTransaction();
			}
			
			if (json) pfree(json);
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"files query failed\"}\n");
		}
		PG_END_TRY();
		return;
	}

	/* Generic handler: /api/:schema/:table */
	pgstar_api_generic_table(req, nseg, segs, resp);
}

static void
pgstar_api_information_schema(int nseg, const char *segs[], PgstarResponse *resp)
{
	char	   *json = NULL;

	if (nseg >= 3 && strcmp(segs[2], "schemata") == 0)
	{
		PG_TRY();
		{
			json = pgstar_json_schemata();
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			FlushErrorState();
			if (json)
				pfree(json);
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"schemata query failed\"}\n");
		}
		PG_END_TRY();
	}
	else if (nseg >= 3 && strcmp(segs[2], "tables") == 0)
	{
		PG_TRY();
		{
			json = pgstar_json_tables();
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			FlushErrorState();
			if (json)
				pfree(json);
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"tables query failed\"}\n");
		}
		PG_END_TRY();
	}
	else
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
	}
}

static void
pgstar_api_exec(const PgstarRequest *req, PgstarResponse *resp)
{
	char	sql_buf[1024];
	char	sp_buf[256];
	bool	has_sql;
	bool	has_sp;
	const char *sql_to_run = NULL;
	StringInfoData sql_dynamic;
	bool	used_dynamic = false;
	MemoryContext caller_ctx = CurrentMemoryContext;
	MemoryContext old_ctx;
	char   *resp_json = NULL;

	if (pg_strcasecmp(req->method, "GET") != 0 &&
		pg_strcasecmp(req->method, "POST") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	has_sql = pgstar_get_query_param(req->query, "sql", sql_buf, sizeof(sql_buf));
	has_sp = pgstar_get_query_param(req->query, "sp", sp_buf, sizeof(sp_buf));

	if (!has_sql && !has_sp)
	{
		resp->status_code = 400;
		resp->status_text = "Bad Request";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"missing sql or sp parameter\"}\n");
		return;
	}

	if (has_sql && sql_buf[0] != '\0')
	{
		sql_to_run = sql_buf;
	}
	else if (has_sp && sp_buf[0] != '\0')
	{
		/* Build a simple SELECT * FROM sp() call */
		initStringInfo(&sql_dynamic);
		appendStringInfo(&sql_dynamic, "SELECT * FROM %s()", sp_buf);
		sql_to_run = sql_dynamic.data;
		used_dynamic = true;
	}
	else
	{
		resp->status_code = 400;
		resp->status_text = "Bad Request";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"empty sql or sp parameter\"}\n");
		return;
	}

	PG_TRY();
	{
		int			ret;
		StringInfoData buf;
		uint64		rows;

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errmsg("pgstar: SPI_connect failed: %d", ret)));

		ret = SPI_execute(sql_to_run, true, 0);
		if (ret < 0)
			ereport(ERROR,
					(errmsg("pgstar: exec query failed: %d", ret)));

		rows = SPI_processed;

		initStringInfo(&buf);
		appendStringInfoString(&buf, "{\"errors\":[],\"rows_affected\":");
		appendStringInfo(&buf, UINT64_FORMAT, rows);
		appendStringInfoString(&buf, ",\"result\":[");

		if (SPI_tuptable != NULL && rows > 0)
		{
			for (uint64 i = 0; i < rows; i++)
			{
				HeapTuple	tup = SPI_tuptable->vals[i];
				TupleDesc	tupdesc = SPI_tuptable->tupdesc;

				if (i > 0)
					appendStringInfoString(&buf, ",");

				appendStringInfoChar(&buf, '{');
				for (int j = 1; j <= tupdesc->natts; j++)
				{
					char	   *colname;
					char	   *val;

					if (j > 1)
						appendStringInfoString(&buf, ",");

					colname = NameStr(TupleDescAttr(tupdesc, j - 1)->attname);
					val = SPI_getvalue(tup, tupdesc, j);
					if (!val)
						val = "null";

					appendStringInfo(&buf, "\"%s\":\"%s\"", colname, val);
				}
				appendStringInfoChar(&buf, '}');
			}
		}

		appendStringInfoString(&buf, "]}\n");

		old_ctx = MemoryContextSwitchTo(caller_ctx);
		resp_json = pstrdup(buf.data);
		MemoryContextSwitchTo(old_ctx);

		pfree(buf.data);

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = resp_json;

		if (used_dynamic && sql_dynamic.data)
			pfree(sql_dynamic.data);
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();
		
		FlushErrorState();
		
		/* Cleanup transaction */
		if (IsTransactionState()) {
			SPI_finish();
			PopActiveSnapshot();
			AbortCurrentTransaction();
		}

		if (used_dynamic && sql_dynamic.data)
			pfree(sql_dynamic.data);

		if (edata)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			appendStringInfoString(&buf, "{\"errors\":[\"");
			appendStringInfoString(&buf, edata->message);
			appendStringInfoString(&buf, "\"],\"rows_affected\":0,\"result\":[]}\n");

			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = buf.data;

			FreeErrorData(edata);
		}
		else
		{
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"errors\":[\"unknown\"],\"rows_affected\":0,\"result\":[]}\n");
		}
	}
	PG_END_TRY();
}

static void
pgstar_api_generic_table(const PgstarRequest *req,
						 int nseg, const char *segs[],
						 PgstarResponse *resp)
{
	const char *schema;
	const char *table;
	StringInfoData sql;
	StringInfoData select_list;
	MemoryContext caller_ctx = CurrentMemoryContext;
	MemoryContext old_ctx;
	char	   *resp_json = NULL;

	if (nseg < 3)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"invalid api path\"}\n");
		return;
	}

	schema = segs[1];
	table = segs[2];

	/* Do not allow generic access to information_schema.* */
	if (strcmp(schema, "information_schema") == 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
		return;
	}

	initStringInfo(&select_list);
	pgstar_api_build_select_list(req->query, &select_list);

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT %s FROM %s.%s",
					 select_list.len > 0 ? select_list.data : "*",
					 quote_identifier(schema),
					 quote_identifier(table));

	PG_TRY();
	{
		int			ret;
		StringInfoData buf;

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errmsg("pgstar: SPI_connect failed: %d", ret)));

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					(errmsg("pgstar: generic table query failed: %d", ret)));

		initStringInfo(&buf);
		appendStringInfoString(&buf, "[");

		for (uint64 i = 0; i < SPI_processed; i++)
		{
			HeapTuple	tup = SPI_tuptable->vals[i];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;

			if (i > 0)
				appendStringInfoString(&buf, ",");

			appendStringInfoChar(&buf, '{');
			for (int j = 1; j <= tupdesc->natts; j++)
			{
				char	   *colname;
				char	   *val;

				if (j > 1)
					appendStringInfoString(&buf, ",");

				colname = NameStr(TupleDescAttr(tupdesc, j - 1)->attname);
				val = SPI_getvalue(tup, tupdesc, j);
				if (!val)
					val = "null";

				appendStringInfo(&buf, "\"%s\":\"%s\"", colname, val);
			}
			appendStringInfoChar(&buf, '}');
		}

		appendStringInfoString(&buf, "]\n");

		old_ctx = MemoryContextSwitchTo(caller_ctx);
		resp_json = pstrdup(buf.data);
		MemoryContextSwitchTo(old_ctx);

		pfree(buf.data);

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = resp_json;
	}
	PG_CATCH();
	{
		FlushErrorState();
		
		/* Cleanup transaction */
		if (IsTransactionState()) {
			SPI_finish();
			PopActiveSnapshot();
			AbortCurrentTransaction();
		}
		
		resp->status_code = 500;
		resp->status_text = "Internal Server Error";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"generic api query failed\"}\n");
	}
	PG_END_TRY();

	if (select_list.data)
		pfree(select_list.data);
	if (sql.data)
		pfree(sql.data);
}

static void
pgstar_api_build_select_list(const char *query, StringInfo select_list)
{
	char		buf[256];
	const char *p;

	if (!pgstar_get_query_param(query, "select", buf, sizeof(buf)))
		return;

	p = buf;

	initStringInfo(select_list);
	while (*p)
	{
		char	ch = *p++;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') ||
			ch == '_' || ch == ',' || ch == ' ')
		{
			appendStringInfoChar(select_list, ch);
		}
	}
}
