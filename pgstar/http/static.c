#include "postgres.h"

#include "pgstar.h"

void
pgstar_route_static(const PgstarRequest *req, PgstarResponse *resp)
{
	/* Placeholder: static file serving can be wired to pgstar_http_send_html_file */
	resp->status_code = 404;
	resp->status_text = "Not Found";
	resp->content_type = "application/json";
	resp->body = pstrdup("{\"error\":\"static content not implemented\"}\n");
}


