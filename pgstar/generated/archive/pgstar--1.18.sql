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


