#include "postgres.h"

#include "http/request.h"

#include "lib/stringinfo.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool
pgstar_parse_http_request(const char *buf, ssize_t len, PgstarRequest *req)
{
	const char *line_end;
	size_t		line_len;
	char		line[512];

	if (buf == NULL || len <= 0 || req == NULL)
		return false;

	memset(req, 0, sizeof(*req));

	line_end = strstr(buf, "\r\n");
	if (line_end == NULL)
		return false;

	line_len = line_end - buf;
	if (line_len >= sizeof(line))
		line_len = sizeof(line) - 1;

	memcpy(line, buf, line_len);
	line[line_len] = '\0';

	if (sscanf(line, "%7s %255s", req->method, req->path) != 2)
		return false;

	return true;
}

void
pgstar_http_request_handle(int client_fd, const char *peek_buf, ssize_t peek_len)
{
	StringInfoData request;
	char		buf[1024];
	ssize_t		nread;
	PgstarRequest req;
	PgstarResponse resp;

	initStringInfo(&request);

	if (peek_buf && peek_len > 0)
		appendBinaryStringInfo(&request, peek_buf, peek_len);

	for (;;)
	{
		nread = recv(client_fd, buf, sizeof(buf), 0);
		if (nread <= 0)
			break;
		appendBinaryStringInfo(&request, buf, nread);
		if (strstr(request.data, "\r\n\r\n") != NULL)
			break;
	}

	elog(pgstar_log_level, "pgstar: http request %.*s",
		 (int) Min(request.len, 120), request.data);

	memset(&resp, 0, sizeof(resp));

	if (!pgstar_parse_http_request(request.data, request.len, &req))
	{
		resp.status_code = 400;
		resp.status_text = "Bad Request";
		resp.content_type = "application/json";
		resp.body = pstrdup("{\"error\":\"bad request\"}\n");
	}
	else
	{
		pgstar_route_request(&req, &resp);
		if (resp.status_code == 0)
		{
			resp.status_code = 500;
			resp.status_text = "Internal Server Error";
			resp.content_type = "application/json";
			resp.body = pstrdup("{\"error\":\"uncategorized\"}\n");
		}
	}

	pgstar_http_send_response(client_fd, &resp);
	if (resp.body)
		pfree(resp.body);

	pfree(request.data);
	close(client_fd);
}
