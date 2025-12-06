#include "postgres.h"

#include "pgstar.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"

#include <string.h>

static void pgstar_api_information_schema(int nseg, const char *segs[],
										  PgstarResponse *resp);
static void pgstar_api_generic_table(const PgstarRequest *req,
									 int nseg, const char *segs[],
									 PgstarResponse *resp);
static void pgstar_api_exec(const PgstarRequest *req, PgstarResponse *resp);
static void pgstar_api_connections(const PgstarRequest *req,
								   int nseg, const char *segs[],
								   PgstarResponse *resp);
static void pgstar_api_build_select_list(const char *query, StringInfo select_list);
static void pgstar_json_append_tuple(StringInfo buf, TupleDesc tupdesc,
									 HeapTuple tuple);

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
		char	   *json = NULL;

		PG_TRY();
		{
			json = pgstar_json_files();
			elog(pgstar_log_level,
				 "pgstar: files response length=%zu bytes",
				 json ? strlen(json) : 0);
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			ErrorData  *edata = CopyErrorData();

			if (edata)
				elog(pgstar_log_level, "pgstar: files query failed: %s", edata->message);

			FlushErrorState();

			if (edata)
				FreeErrorData(edata);

			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"files query failed\"}\n");
		}
		PG_END_TRY();
		return;
	}

	/* /api/pgstar/connections endpoint - special handling for POST/PUT */
	if (nseg >= 3 && strcmp(segs[1], "pgstar") == 0 && strcmp(segs[2], "connections") == 0)
	{
		pgstar_api_connections(req, nseg, segs, resp);
		return;
	}

	/* Generic handler: /api/:schema/:table */
	pgstar_api_generic_table(req, nseg, segs, resp);
}

static void
pgstar_json_append_tuple(StringInfo buf, TupleDesc tupdesc, HeapTuple tuple)
{
	bool		first = true;
	int			natts;
	int			attnum;
	Oid			output_func_oid;
	bool		output_func_isvarlena;

	/* Safety checks */
	if (buf == NULL || tupdesc == NULL || tuple == NULL)
		ereport(ERROR,
				(errmsg("pgstar: invalid parameters to pgstar_json_append_tuple")));

	/* Capture natts once before loop */
	natts = tupdesc->natts;
	if (natts < 0 || natts > 10000)
		ereport(ERROR,
				(errmsg("pgstar: invalid natts value %d in tuple descriptor", natts)));

	appendStringInfoChar(buf, '{');
	for (attnum = 1; attnum <= natts; attnum++)
	{
		Form_pg_attribute attr;
		Oid			atttypid;
		bool		isnull;
		Datum		attr_datum;
		char	   *value = NULL;

		/* Safety checks */
		if (attnum < 1 || attnum > natts)
			continue;

		attr = TupleDescAttr(tupdesc, attnum - 1);
		if (attr == NULL || attr->attisdropped)
			continue;

		atttypid = attr->atttypid;

		if (!first)
			appendStringInfoChar(buf, ',');
		first = false;

		escape_json(buf, NameStr(attr->attname));
		appendStringInfoChar(buf, ':');

		/* SAFE PATTERN: Use SPI_getbinval() → Datum → output function */
		/* This avoids SPI_getvalue() which can return corrupted pointers */
		attr_datum = SPI_getbinval(tuple, tupdesc, attnum, &isnull);

		if (isnull)
		{
			appendStringInfoString(buf, "null");
			PGSTAR_VERBOSE_LOG("api_tuple: attr=%s NULL", NameStr(attr->attname));
		}
		else
		{
			/* Get the type's output function - this is safe and guaranteed to work */
			getTypeOutputInfo(atttypid, &output_func_oid, &output_func_isvarlena);

			/* Call the output function to convert Datum to C string */
			/* This will either return a valid string or raise a normal Postgres ERROR */
			value = OidOutputFunctionCall(output_func_oid, attr_datum);

			if (value == NULL)
			{
				appendStringInfoString(buf, "null");
				PGSTAR_VERBOSE_LOG("api_tuple: attr=%s output function returned NULL", NameStr(attr->attname));
			}
			else
			{
				PGSTAR_VERBOSE_LOG("api_tuple: attr=%s type=%u",
								   NameStr(attr->attname),
								   atttypid);

				/* Handle different types appropriately */
				switch (atttypid)
				{
					case BOOLOID:
						/* Boolean output is already "t" or "f", convert to JSON boolean */
						if (value[0] == 't')
							appendStringInfoString(buf, "true");
						else
							appendStringInfoString(buf, "false");
						break;
					case INT2OID:
					case INT4OID:
					case INT8OID:
					case OIDOID:
					case FLOAT4OID:
					case FLOAT8OID:
					case NUMERICOID:
						/* Numeric types output is already valid JSON number */
						appendStringInfoString(buf, value);
						break;
					case JSONOID:
					case JSONBOID:
						/* JSON/JSONB output is already valid JSON */
						appendStringInfoString(buf, value);
						break;
					default:
						/* String types: escape for JSON (adds quotes) */
						escape_json(buf, value);
						break;
				}

				pgstar_safe_pfree(value);
			}
		}
	}
	appendStringInfoChar(buf, '}');
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
			elog(pgstar_log_level,
				 "pgstar: info_schema schemata response length=%zu bytes",
				 json ? strlen(json) : 0);
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			FlushErrorState();
			if (json)
				pgstar_safe_pfree(json);
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
			elog(pgstar_log_level,
				 "pgstar: info_schema tables response length=%zu bytes",
				 json ? strlen(json) : 0);
			resp->status_code = 200;
			resp->status_text = "OK";
			resp->content_type = "application/json";
			resp->body = json;
		}
		PG_CATCH();
		{
			FlushErrorState();
			if (json)
				pgstar_safe_pfree(json);
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
		TupleDesc tupdesc;
		uint64		rows;
		uint64		i;

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
			tupdesc = SPI_tuptable->tupdesc;
			if (tupdesc == NULL)
				ereport(ERROR,
						(errmsg("pgstar: SPI_tuptable->tupdesc is NULL in api_exec")));

			for (i = 0; i < rows; i++)
			{
				if (i > 0)
					appendStringInfoChar(&buf, ',');

				if (SPI_tuptable->vals == NULL ||
					SPI_tuptable->vals[i] == NULL)
					ereport(ERROR,
							(errmsg("pgstar: NULL tuple at index " UINT64_FORMAT " in api_exec", i)));

				pgstar_json_append_tuple(&buf, tupdesc, SPI_tuptable->vals[i]);
			}
		}

		appendStringInfoString(&buf, "]}\n");

		old_ctx = MemoryContextSwitchTo(caller_ctx);
		resp_json = pstrdup(buf.data);
		MemoryContextSwitchTo(old_ctx);

		pgstar_safe_pfree(buf.data);

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = resp_json;

		if (used_dynamic && sql_dynamic.data)
			pgstar_safe_pfree(sql_dynamic.data);
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();
		MemoryContext old_ctx;

		FlushErrorState();

		/* Cleanup transaction if still active */
		if (IsTransactionState())
		{
			/* Best-effort cleanup; ignore errors from these. */
			(void) SPI_finish();
			PopActiveSnapshot();
			AbortCurrentTransaction();
		}

		if (used_dynamic && sql_dynamic.data)
			pgstar_safe_pfree(sql_dynamic.data);

		old_ctx = MemoryContextSwitchTo(caller_ctx);

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

		MemoryContextSwitchTo(old_ctx);
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
		TupleDesc tupdesc;
		uint64 tuple_count;
		uint64 i;

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

		/* Safety check: ensure SPI_tuptable is valid */
		if (SPI_tuptable == NULL)
			ereport(ERROR,
					(errmsg("pgstar: SPI_tuptable is NULL after query")));

		/* Capture tuple descriptor and tuple count immediately - they're valid until SPI_finish() */
		tupdesc = SPI_tuptable->tupdesc;
		tuple_count = SPI_processed;

		if (tupdesc == NULL)
			ereport(ERROR,
					(errmsg("pgstar: SPI_tuptable->tupdesc is NULL after query")));

		if (SPI_tuptable->vals == NULL)
			ereport(ERROR,
					(errmsg("pgstar: SPI_tuptable->vals is NULL after query")));

		/* Initialize StringInfo buffer - ensure it's in the transaction memory context */
		initStringInfo(&buf);
		if (buf.data == NULL)
			ereport(ERROR,
					(errmsg("pgstar: StringInfo initialization failed")));
		
		appendStringInfoChar(&buf, '[');

		for (i = 0; i < tuple_count; i++)
		{
			if (i > 0)
				appendStringInfoChar(&buf, ',');

			if (i >= (uint64) SPI_tuptable->numvals)
				ereport(ERROR,
						(errmsg("pgstar: invalid tuple index " UINT64_FORMAT
								" (numvals=%d)",
								i, (int) SPI_tuptable->numvals)));

			if (SPI_tuptable->vals[i] == NULL)
				ereport(ERROR,
						(errmsg("pgstar: tuple at index " UINT64_FORMAT " is NULL", i)));

			pgstar_json_append_tuple(&buf, tupdesc, SPI_tuptable->vals[i]);
		}

		appendStringInfoChar(&buf, ']');
		appendStringInfoChar(&buf, '\n');

		old_ctx = MemoryContextSwitchTo(caller_ctx);
		resp_json = pstrdup(buf.data);
		MemoryContextSwitchTo(old_ctx);

		pgstar_safe_pfree(buf.data);

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
		ErrorData  *edata = CopyErrorData();
		MemoryContext old_ctx;

		if (edata)
			elog(pgstar_log_level, "pgstar: generic API query failed: %s", edata->message);

		FlushErrorState();
		
		/* Cleanup transaction */
		if (IsTransactionState()) {
			SPI_finish();
			PopActiveSnapshot();
			AbortCurrentTransaction();
		}
		
		old_ctx = MemoryContextSwitchTo(caller_ctx);

		if (edata && edata->sqlerrcode == ERRCODE_UNDEFINED_TABLE)
		{
			resp->status_code = 404;
			resp->status_text = "Not Found";
			resp->content_type = "application/json";
			resp->body = psprintf("{\"error\":\"relation %s.%s not found\"}\n",
								  schema, table);
		}
		else
		{
			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"generic api query failed\"}\n");
		}

		MemoryContextSwitchTo(old_ctx);

		if (edata)
			FreeErrorData(edata);
	}
	PG_END_TRY();

	if (select_list.data)
		pgstar_safe_pfree(select_list.data);
	if (sql.data)
		pgstar_safe_pfree(sql.data);
}

static void
pgstar_api_build_select_list(const char *query, StringInfo select_list)
{
	char		buf[256];
	const char *p;

	if (!pgstar_get_query_param(query, "select", buf, sizeof(buf)))
		return;

	p = buf;

	/* select_list is already initialized by caller */
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

static void
pgstar_api_connections(const PgstarRequest *req,
					   int nseg, const char *segs[],
					   PgstarResponse *resp)
{
	MemoryContext caller_ctx = CurrentMemoryContext;
	MemoryContext old_ctx;
	char	   *resp_json = NULL;
	StringInfoData sql;
	StringInfoData body_buf;

	if (pg_strcasecmp(req->method, "POST") == 0)
	{
		/* POST: Insert new connection */
		if (!req->body || req->body_len == 0)
		{
			resp->status_code = 400;
			resp->status_text = "Bad Request";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"missing request body\"}\n");
			return;
		}

		PG_TRY();
		{
			int			ret;
			TupleDesc tupdesc;
			uint64		tuple_count;

			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());

			ret = SPI_connect();
			if (ret != SPI_OK_CONNECT)
				ereport(ERROR,
						(errmsg("pgstar: SPI_connect failed: %d", ret)));

			/* Build SQL using jsonb functions to parse the body */
			initStringInfo(&body_buf);
			appendStringInfo(&body_buf, "%.*s", (int) req->body_len, req->body);

			{
				Datum		quoted_body;
				char	   *quoted_body_str;

				quoted_body = DirectFunctionCall1(quote_literal,
												   CStringGetDatum(body_buf.data));
				quoted_body_str = DatumGetCString(quoted_body);

				initStringInfo(&sql);
				appendStringInfo(&sql,
								 "INSERT INTO pgstar.connections (name, fdw_type, connection_string, host, port, db_name, \"user\", pass_b64, fdw_version) "
								 "SELECT "
								 "  (body->>'name')::text, "
								 "  COALESCE((body->>'fdw_type')::text, 'postgres_fdw'), "
								 "  COALESCE((body->>'connection_string')::text, ''), "
								 "  COALESCE((body->>'host')::text, ''), "
								 "  CASE WHEN (body->>'port') IS NOT NULL AND (body->>'port')::text != '' THEN ((body->>'port')::text)::integer ELSE NULL END, "
								 "  COALESCE((body->>'db_name')::text, ''), "
								 "  COALESCE((body->>'user')::text, ''), "
								 "  COALESCE((body->>'pass_b64')::text, ''), "
								 "  COALESCE((body->>'fdw_version')::text, '') "
								 "FROM (SELECT %s::jsonb AS body) AS j "
								 "RETURNING id",
								 quoted_body_str);
			}

			ret = SPI_execute(sql.data, false, 0);
			if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
				ereport(ERROR,
						(errmsg("pgstar: connections insert failed: %d", ret)));

			tuple_count = SPI_processed;
			if (tuple_count > 0 && SPI_tuptable != NULL)
			{
				HeapTuple	tup = SPI_tuptable->vals[0];
				TupleDesc	tupdesc = SPI_tuptable->tupdesc;
				bool		isnull;
				Datum		id_datum;
				int32		id;

				id_datum = SPI_getbinval(tup, tupdesc, 1, &isnull);
				if (!isnull)
					id = DatumGetInt32(id_datum);
				else
					id = 0;

				old_ctx = MemoryContextSwitchTo(caller_ctx);
				resp_json = psprintf("{\"success\":true,\"id\":%d}\n", id);
				MemoryContextSwitchTo(old_ctx);
			}
			else
			{
				old_ctx = MemoryContextSwitchTo(caller_ctx);
				resp_json = pstrdup("{\"success\":true}\n");
				MemoryContextSwitchTo(old_ctx);
			}

			pgstar_safe_pfree(sql.data);
			pgstar_safe_pfree(body_buf.data);

			SPI_finish();
			PopActiveSnapshot();
			CommitTransactionCommand();

			resp->status_code = 201;
			resp->status_text = "Created";
			resp->content_type = "application/json";
			resp->body = resp_json;
		}
		PG_CATCH();
		{
			ErrorData  *edata = CopyErrorData();

			FlushErrorState();

			if (IsTransactionState())
			{
				SPI_finish();
				PopActiveSnapshot();
				AbortCurrentTransaction();
			}

			if (sql.data)
				pgstar_safe_pfree(sql.data);
			if (body_buf.data)
				pgstar_safe_pfree(body_buf.data);

			old_ctx = MemoryContextSwitchTo(caller_ctx);

			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = psprintf("{\"error\":\"%s\"}\n",
								  edata ? edata->message : "insert failed");

			MemoryContextSwitchTo(old_ctx);

			if (edata)
				FreeErrorData(edata);
		}
		PG_END_TRY();
		return;
	}
	else if (pg_strcasecmp(req->method, "PUT") == 0 && nseg == 4)
	{
		/* PUT: Update connection by ID */
		int			conn_id = atoi(segs[3]);

		if (conn_id <= 0)
		{
			resp->status_code = 400;
			resp->status_text = "Bad Request";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"invalid connection id\"}\n");
			return;
		}

		if (!req->body || req->body_len == 0)
		{
			resp->status_code = 400;
			resp->status_text = "Bad Request";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"missing request body\"}\n");
			return;
		}

		PG_TRY();
		{
			int			ret;

			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());

			ret = SPI_connect();
			if (ret != SPI_OK_CONNECT)
				ereport(ERROR,
						(errmsg("pgstar: SPI_connect failed: %d", ret)));

			initStringInfo(&body_buf);
			appendStringInfo(&body_buf, "%.*s", (int) req->body_len, req->body);

			{
				Datum		quoted_body;
				char	   *quoted_body_str;

				quoted_body = DirectFunctionCall1(quote_literal,
												   CStringGetDatum(body_buf.data));
				quoted_body_str = DatumGetCString(quoted_body);

				initStringInfo(&sql);
				appendStringInfo(&sql,
								 "UPDATE pgstar.connections SET "
								 "  name = COALESCE(NULLIF((body->>'name')::text, ''), name), "
								 "  fdw_type = COALESCE(NULLIF((body->>'fdw_type')::text, ''), fdw_type), "
								 "  connection_string = COALESCE(NULLIF((body->>'connection_string')::text, ''), connection_string), "
								 "  host = COALESCE(NULLIF((body->>'host')::text, ''), host), "
								 "  port = CASE WHEN (body->>'port') IS NOT NULL AND (body->>'port')::text != '' THEN ((body->>'port')::text)::integer ELSE port END, "
								 "  db_name = COALESCE(NULLIF((body->>'db_name')::text, ''), db_name), "
								 "  \"user\" = COALESCE(NULLIF((body->>'user')::text, ''), \"user\"), "
								 "  pass_b64 = COALESCE(NULLIF((body->>'pass_b64')::text, ''), pass_b64), "
								 "  fdw_version = COALESCE(NULLIF((body->>'fdw_version')::text, ''), fdw_version), "
								 "  updated_at = CURRENT_TIMESTAMP "
								 "FROM (SELECT %s::jsonb AS body) AS j "
								 "WHERE id = %d "
								 "RETURNING id",
								 quoted_body_str,
								 conn_id);
			}

			ret = SPI_execute(sql.data, false, 0);
			if (ret != SPI_OK_UPDATE)
				ereport(ERROR,
						(errmsg("pgstar: connections update failed: %d", ret)));

			old_ctx = MemoryContextSwitchTo(caller_ctx);
			resp_json = pstrdup("{\"success\":true}\n");
			MemoryContextSwitchTo(old_ctx);

			pgstar_safe_pfree(sql.data);
			pgstar_safe_pfree(body_buf.data);

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
			ErrorData  *edata = CopyErrorData();

			FlushErrorState();

			if (IsTransactionState())
			{
				SPI_finish();
				PopActiveSnapshot();
				AbortCurrentTransaction();
			}

			if (sql.data)
				pgstar_safe_pfree(sql.data);
			if (body_buf.data)
				pgstar_safe_pfree(body_buf.data);

			old_ctx = MemoryContextSwitchTo(caller_ctx);

			resp->status_code = 500;
			resp->status_text = "Internal Server Error";
			resp->content_type = "application/json";
			resp->body = psprintf("{\"error\":\"%s\"}\n",
								  edata ? edata->message : "update failed");

			MemoryContextSwitchTo(old_ctx);

			if (edata)
				FreeErrorData(edata);
		}
		PG_END_TRY();
		return;
	}
	
	/* Fall through to generic handler for GET */
	pgstar_api_generic_table(req, nseg, segs, resp);
}
