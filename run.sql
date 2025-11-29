DROP EXTENSION IF EXISTS pgstar CASCADE;
CREATE EXTENSION pgstar;
-- LEGACY/REF
-- ALTER EXTENSION pgstar UPDATE; -- DOES NOT ALWAYS UPDATE

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

-- select page_name, parent_tag from pgstar.pages WHERE page_name='dashboard';

select * from pgstar.jobs;
select * from pgstar.pages;
select * from pgstar.files;

-- DELETE FROM pgstar.pages; ˀx  