#include "postgres.h"

#include "http/request.h"

#include "lib/stringinfo.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void pgstar_http_request_send_response(int client_fd);

void
pgstar_http_request_handle(int client_fd, const char *peek_buf, ssize_t peek_len)
{
	StringInfoData request;
	char		buf[1024];
	ssize_t		nread;

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

	elog(DEBUG1, "pgstar: http request %.*s",
		 (int) Min(request.len, 120), request.data);

	pgstar_http_request_send_response(client_fd);

	pfree(request.data);
	close(client_fd);
}

static void
pgstar_http_request_send_response(int client_fd)
{
	static const char *body = "pgstar background worker";
	char		response[256];
	int			len = snprintf(response, sizeof(response),
							 "HTTP/1.1 200 OK\r\n"
							 "Content-Type: text/plain\r\n"
							 "Content-Length: %zu\r\n"
							 "Connection: close\r\n"
							 "\r\n"
							 "%s",
							 strlen(body),
							 body);

	if (len < 0)
		return;

	(void) send(client_fd, response, len, 0);
}

