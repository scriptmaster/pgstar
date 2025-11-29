#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>

void
pgstar_route_request(const PgstarRequest *req, PgstarResponse *resp)
{
	memset(resp, 0, sizeof(*resp));

	if (req == NULL)
	{
		resp->status_code = 500;
		resp->status_text = "Internal Server Error";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"missing request\"}\n");
		return;
	}

	/* Always capture a simple log line for X-Log header */
	resp->log = psprintf("method=%s uri=%s path=%s",
						 req->method,
						 req->uri,
						 req->path);
	/* Root (/) is a hardcoded embedded HTML page */
	if (strcmp(req->path, "/") == 0)
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "text/html; charset=utf-8";
		resp->body = pstrdup(pgstar_root_html);
		return;
	}
	else
	/* /app/ paths are handled by the UI pages router */
	if (strncmp(req->path, "/app/", 5) == 0 ||
		strcmp(req->path, "/app") == 0)
	{
		pgstar_route_ui_pages(req, resp);
		return;
	}
	else
	if (strncmp(req->path, "/pgstar/", 8) == 0)
	{
		pgstar_route_pgstar(req, resp);
		return;
	}
	else
	/* /static/js/ and /static/css/ are served via the UI asset routers */
	if (strncmp(req->path, "/static/js/", 11) == 0)
	{
		pgstar_route_ui_js(req, resp);
		return;
	}
	else
	if (strncmp(req->path, "/static/css/", 12) == 0)
	{
		pgstar_route_ui_css(req, resp);
		return;
	}
	else
	/* /api/ allows both GET and POST (and potentially others) */
	if (strncmp(req->path, "/api/", 5) == 0)
	{
		pgstar_route_api(req, resp);
		return;
	}
	else
	/* /v1/ paths are routed to AI chat handler */
	if (strncmp(req->path, "/v1/", 4) == 0)
	{
		pgstar_route_v1(req, resp);
		return;
	}
	else
	/* /ai/ is delegated to the AI router; methods are not restricted here */
	if (strncmp(req->path, "/ai/", 4) == 0)
	{
		pgstar_route_ai(req, resp);
		return;
	}
	else
	/* /static/ disallows POST, other methods are passed through */
	if (strncmp(req->path, "/static/", 8) == 0)
	{
		if (pg_strcasecmp(req->method, "POST") == 0)
		{
			resp->status_code = 405;
			resp->status_text = "Method Not Allowed";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		}
		else
		{
			pgstar_route_static(req, resp);
		}
		return;
	}
	else
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
	}
}

