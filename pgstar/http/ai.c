#include "postgres.h"

#include "pgstar.h"

void
pgstar_route_ai(const PgstarRequest *req, PgstarResponse *resp)
{
	/* For now, just return a placeholder JSON payload. */
	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = "application/json";
	resp->body = pstrdup("{\"service\":\"pgstar\",\"ai\":\"not implemented yet\"}\n");
}


