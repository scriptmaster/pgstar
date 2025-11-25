#include "postgres.h"
#include "fmgr.h"

#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

PG_FUNCTION_INFO_V1(pgstart_create_job);
PG_FUNCTION_INFO_V1(pgstar_job_status);

static void pgstar_jobs_ensure_spi_connect(void);

Datum
pgstart_create_job(PG_FUNCTION_ARGS)
{
	text	   *job_type_text;
	text	   *cron_text;
	text	   *table_name_text;
	text	   *key_column_text;
	text	   *given_stmt_text;
	Jsonb	   *params = NULL;
	text	   *range_direction_text;
	char		isnull[10];
	Datum		values[10];
	Oid			argtypes[10];
	char	   *job_type;
	char	   *cron_expr = NULL;
	char	   *table_name;
	char	   *key_column;
	char	   *given_stmt;
	char	   *range_direction;
	Datum		range_start;
	Datum		range_end;
	int32		batch_count;
	int64		job_id = -1;
	int			ret;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(2) || PG_ARGISNULL(3) ||
		PG_ARGISNULL(4) || PG_ARGISNULL(6) || PG_ARGISNULL(7))
		ereport(ERROR,
				(errmsg("pgstart_create_job: required arguments must not be NULL")));

	job_type_text = PG_GETARG_TEXT_PP(0);
	cron_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);
	table_name_text = PG_GETARG_TEXT_PP(2);
	key_column_text = PG_GETARG_TEXT_PP(3);
	given_stmt_text = PG_GETARG_TEXT_PP(4);
	params = PG_ARGISNULL(5) ? NULL : PG_GETARG_JSONB_P(5);
	range_direction_text = PG_GETARG_TEXT_PP(6);
	range_start = PG_GETARG_DATUM(7);
	range_end = PG_ARGISNULL(8) ? (Datum) 0 : PG_GETARG_DATUM(8);
	batch_count = PG_ARGISNULL(9) ? 1000 : PG_GETARG_INT32(9);

	job_type = text_to_cstring(job_type_text);
	table_name = text_to_cstring(table_name_text);
	key_column = text_to_cstring(key_column_text);
	given_stmt = text_to_cstring(given_stmt_text);
	range_direction = text_to_cstring(range_direction_text);

	if (cron_text != NULL)
		cron_expr = text_to_cstring(cron_text);

	if (strcmp(job_type, "run_once") != 0 &&
		strcmp(job_type, "cron") != 0 &&
		strcmp(job_type, "continuous") != 0)
		ereport(ERROR,
				(errmsg("pgstart_create_job: invalid job_type \"%s\"", job_type)));

	if (strcmp(range_direction, "asc") != 0 &&
		strcmp(range_direction, "desc") != 0)
		ereport(ERROR,
				(errmsg("pgstart_create_job: invalid range_direction \"%s\"", range_direction)));

	if (batch_count <= 0)
		batch_count = 1000;

	pgstar_jobs_ensure_spi_connect();

	memset(values, 0, sizeof(values));
	memset(isnull, '\0', sizeof(isnull));
	memset(argtypes, 0, sizeof(argtypes));

	argtypes[0] = TEXTOID;
	values[0] = CStringGetTextDatum(job_type);

	argtypes[1] = TEXTOID;
	if (cron_expr)
		values[1] = CStringGetTextDatum(cron_expr);
	else
		isnull[1] = 'n';

	argtypes[2] = TEXTOID;
	values[2] = CStringGetTextDatum(table_name);

	argtypes[3] = TEXTOID;
	values[3] = CStringGetTextDatum(key_column);

	argtypes[4] = TEXTOID;
	values[4] = CStringGetTextDatum(given_stmt);

	argtypes[5] = JSONBOID;
	if (params)
		values[5] = JsonbPGetDatum(params);
	else
		isnull[5] = 'n';

	argtypes[6] = TEXTOID;
	values[6] = CStringGetTextDatum(range_direction);

	argtypes[7] = NUMERICOID;
	values[7] = range_start;

	argtypes[8] = NUMERICOID;
	if (PG_ARGISNULL(8))
		isnull[8] = 'n';
	else
		values[8] = range_end;

	argtypes[9] = INT4OID;
	values[9] = Int32GetDatum(batch_count);

	ret = SPI_execute_with_args(
		"INSERT INTO pgstar.jobs ("
		"job_type, cron_expression, table_name, key_column, given_stmt, "
		"parameters, range_direction, start_range, end_range, batch_count"
		") VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) "
		"RETURNING job_id",
		10,
		argtypes,
		values,
		isnull,
		false,
		0);

	if (ret != SPI_OK_INSERT_RETURNING || SPI_processed != 1)
		ereport(ERROR,
				(errmsg("pgstart_create_job: failed to insert job (code %d, rows " UINT64_FORMAT ")",
						ret, (uint64) SPI_processed)));

	{
		bool		isnull_bool;

		job_id = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
											 SPI_tuptable->tupdesc,
											 1,
											 &isnull_bool));
		isnull[0] = isnull_bool ? 'n' : '\0';
	}

	SPI_finish();

	PG_RETURN_INT64(job_id);
}

Datum
pgstar_job_status(PG_FUNCTION_ARGS)
{
	int64		job_id;
	StringInfoData sql;
	int			ret;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errmsg("pgstar_job_status: job_id must not be NULL")));

	job_id = PG_GETARG_INT64(0);

	pgstar_jobs_ensure_spi_connect();

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT jsonb_build_object("
					 "  'job', to_jsonb(j),"
					 "  'runs', COALESCE(jsonb_agg(to_jsonb(r)) "
					 "           FILTER (WHERE r.job_run_id IS NOT NULL), '[]'::jsonb)"
					 " ) AS status "
					 "FROM pgstar.jobs j "
					 "LEFT JOIN pgstar.job_runs r ON r.job_id = j.job_id "
					 "WHERE j.job_id = %lld "
					 "GROUP BY j.job_id",
					 (long long) job_id);

	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
				(errmsg("pgstar_job_status: SPI_execute failed with code %d", ret)));

	if (SPI_processed == 0)
	{
		SPI_finish();
		PG_RETURN_NULL();
	}

	{
		bool	isnull;
		Datum	val;

		val = SPI_getbinval(SPI_tuptable->vals[0],
							SPI_tuptable->tupdesc,
							1,
							&isnull);

		if (isnull)
		{
			SPI_finish();
			PG_RETURN_NULL();
		}

		SPI_finish();
		PG_RETURN_DATUM(val);
	}
}

static void
pgstar_jobs_ensure_spi_connect(void)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errmsg("pgstar jobs: SPI_connect failed")));
}


