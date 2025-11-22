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

	if (strcmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	if (strcmp(req->path, "/") == 0)
	{
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"service\":\"pgstar\",\"status\":\"ok\"}\n");
	}
	else if (strcmp(req->path, "/pgstar/version") == 0)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		appendStringInfo(&buf,
						 "{\"service\":\"pgstar\",\"version\":\"%s\"}\n",
						 PGSTAR_VERSION);

		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = buf.data;
	}
	else if (strcmp(req->path, "/api/information_schema/schemata") == 0)
	{
		char	   *json = NULL;

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
	else if (strcmp(req->path, "/api/information_schema/tables") == 0)
	{
		char	   *json = NULL;

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

