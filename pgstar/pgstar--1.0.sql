-- Install script for the pgstar extension

CREATE OR REPLACE FUNCTION pgstar_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pgstar_version'
LANGUAGE C
STRICT
STABLE;

