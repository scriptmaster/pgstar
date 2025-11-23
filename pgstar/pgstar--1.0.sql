-- Install script for the pgstar extension

CREATE OR REPLACE FUNCTION pgstar_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_version'
LANGUAGE C
STRICT
STABLE;

CREATE OR REPLACE FUNCTION pgstar_http_post(
	url text,
	headers jsonb,
	body text
)
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_http_post'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstar_http_get(
	url text,
	headers jsonb
)
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_http_get'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstart_create_job(
	job_type text,
	cron_expression text,
	table_name text,
	key_column text,
	given_stmt text,
	parameters jsonb,
	range_direction text,
	start_range numeric,
	end_range numeric DEFAULT NULL,
	batch_count integer DEFAULT 1000
)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgstart_create_job'
LANGUAGE C
VOLATILE;

CREATE OR REPLACE FUNCTION pgstar_job_status(
	job_id bigint
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'pgstar_job_status'
LANGUAGE C
STABLE;

