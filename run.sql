-- DROP EXTENSION IF EXISTS pgstar CASCADE;
CREATE EXTENSION pgstar;
ALTER EXTENSION pgstar UPDATE;

SELECT pgstar_version();
-- SELECT pgstar_tcp_server();
-- SELECT pgstar_http_server();

-- select * from pgstar_http_post(
--   'http://httpbin.org/post',
--   '{"Content-Type": "application/json"}'::jsonb,
--   '{"echo":"hello from pgstar"}'
-- );

-- SELECT count(*) from (select * from pgstar_http_post(
--   'http://httpbin.org/post',
--   '{"Content-Type": "application/json"}'::jsonb,
--   '{"echo":"hello from pgstar"}'
-- )) t;

-- select pgstar_http_get::jsonb from pgstar_http_get(
--   'http://httpbin.org/get?param1=one&one=neo&neo=vim',
--   '{"Content-Type": "application/json"}'::jsonb
-- );

-- select * from pgstar.pages;
-- select * from pgstar.jobs;
-- DELETE FROM pgstar.pages;
