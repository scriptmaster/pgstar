#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>

void
pgstar_route_pgstar(const PgstarRequest *req, PgstarResponse *resp)
{
	/* For now we only support /pgstar/version */
	if (strcmp(req->path, "/pgstar/version") == 0)
	{
		if (pg_strcasecmp(req->method, "GET") != 0)
		{
			resp->status_code = 405;
			resp->status_text = "Method Not Allowed";
			resp->content_type = "application/json";
			resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		}
		else
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
	}
	else
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"not found\"}\n");
	}
}


