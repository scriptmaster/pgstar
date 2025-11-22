-- Install script for the hello_world extension

CREATE OR REPLACE FUNCTION hello_world()
RETURNS text
AS 'MODULE_PATHNAME', 'hello_world'
LANGUAGE C
STRICT
STABLE;

