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

	/* Note: body extraction is done in pgstar_http_request_handle */
	memset(req, 0, sizeof(*req));

	line_end = strstr(buf, "\r\n");
	if (line_end == NULL)
		return false;

	line_len = line_end - buf;
	if (line_len >= sizeof(line))
		line_len = sizeof(line) - 1;

	memcpy(line, buf, line_len);
	line[line_len] = '\0';

	if (sscanf(line, "%7s %255s", req->method, req->uri) != 2)
		return false;

	/* Split uri into path/query/hash */
	if (!pgstar_parse_uri(req->uri, req))
		return false;

	return true;
}

void
pgstar_http_request_handle(int client_fd, const char *peek_buf, ssize_t peek_len)
{
	StringInfoData request;
	char		buf[32768]; /* 32KB chunks */
	ssize_t		nread;
	const size_t max_request_size = 256 * 1024; /* hard cap */
	bool		header_done = false;
	int			content_length = 0;
	int			header_bytes = 0;
	PgstarRequest req;
	PgstarResponse resp;

	initStringInfo(&request);
	/* Pre-allocate enough space for typical GET headers (~8KB) */
	enlargeStringInfo(&request, 8192);

	/* 
	 * Note: We do NOT append peek_buf here because it was read with MSG_PEEK,
	 * meaning the data is still in the socket buffer. The recv() loop below
	 * will read it again. Appending it here would duplicate the data.
	 */

	for (;;)
	{
		nread = recv(client_fd, buf, sizeof(buf), 0);
		if (nread <= 0)
			break;
		appendBinaryStringInfo(&request, buf, nread);

		/* Guard against overly large requests (mainly POST bodies) */
		if (request.len >= max_request_size)
		{
			elog(WARNING, "pgstar: request exceeded %zu bytes, aborting", max_request_size);
			break;
		}

		/* Once we have headers, extract Content-Length if present */
		if (!header_done)
		{
			char   *header_end = strstr(request.data, "\r\n\r\n");

			if (header_end != NULL)
			{
				char   *p = request.data;

				header_done = true;
				header_bytes = (int) ((header_end - request.data) + 4);

				while (p < header_end)
				{
					if (pg_strncasecmp(p, "Content-Length:", 15) == 0)
					{
						long		v;

						p += 15;
						while (p < header_end && (*p == ' ' || *p == '\t'))
							p++;

						v = strtol(p, NULL, 10);
						if (v > 0 && v < (long) max_request_size)
							content_length = (int) v;
						break;
					}

					/* advance to next line */
					p = strchr(p, '\n');
					if (p == NULL || p >= header_end)
						break;
					p++;
				}
			}
		}

		/* Stop when we've read headers + full body (if any) */
		if (header_done && request.len >= header_bytes + content_length)
			break;
	}

	elog(pgstar_log_level, "pgstar: http request %.*s",
		 (int) Min(request.len, 120), request.data);

	memset(&resp, 0, sizeof(resp));
	memset(&req, 0, sizeof(req));

	if (!pgstar_parse_http_request(request.data, request.len, &req))
	{
		resp.status_code = 400;
		resp.status_text = "Bad Request";
		resp.content_type = "application/json";
		resp.body = pstrdup("{\"error\":\"bad request\"}\n");
	}
	else
	{
		/* Extract request body */
		if (header_done && content_length > 0 && request.len >= header_bytes + content_length)
		{
			req.body = request.data + header_bytes;
			req.body_len = content_length;
		}
		else
		{
			req.body = NULL;
			req.body_len = 0;
		}
		
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
	if (resp.log)
		pfree(resp.log);

	pfree(request.data);
}
