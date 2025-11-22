#include "postgres.h"

#include "tcp/protocol.h"

#include "http/request.h"
#include "tcp/protocols/other.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PGSTAR_PROTOCOL_PEEK 256
#define PGSTAR_PROTOCOL_TIMEOUT_MS 200

static bool pgstar_tcp_protocol_wait_for_data(int client_fd);
static bool pgstar_tcp_protocol_is_http(const char *buf, ssize_t len);

void
pgstar_tcp_protocol_route(int client_fd)
{
	char		peek[PGSTAR_PROTOCOL_PEEK];
	ssize_t		received;

	if (!pgstar_tcp_protocol_wait_for_data(client_fd))
	{
		pgstar_other_protocol_handle(client_fd, NULL, 0);
		return;
	}

	received = recv(client_fd, peek, sizeof(peek) - 1, MSG_PEEK);
	if (received <= 0)
	{
		pgstar_other_protocol_handle(client_fd, NULL, 0);
		return;
	}

	peek[received] = '\0';

	if (pgstar_tcp_protocol_is_http(peek, received))
		pgstar_http_request_handle(client_fd, peek, received);
	else
		pgstar_other_protocol_handle(client_fd, peek, received);
}

static bool
pgstar_tcp_protocol_wait_for_data(int client_fd)
{
	fd_set		rfds;
	struct timeval tv;
	int			rc;

	FD_ZERO(&rfds);
	FD_SET(client_fd, &rfds);

	tv.tv_sec = PGSTAR_PROTOCOL_TIMEOUT_MS / 1000;
	tv.tv_usec = (PGSTAR_PROTOCOL_TIMEOUT_MS % 1000) * 1000;

	rc = select(client_fd + 1, &rfds, NULL, NULL, &tv);
	if (rc <= 0)
		return false;

	return FD_ISSET(client_fd, &rfds);
}

static bool
pgstar_tcp_protocol_is_http(const char *buf, ssize_t len)
{
	static const char *http_methods[] = {"GET", "POST", "PUT", "HEAD",
	"DELETE", "OPTIONS", "PATCH", "TRACE"};
	int			i;

	for (i = 0; i < lengthof(http_methods); i++)
	{
		size_t		method_len = strlen(http_methods[i]);

		if (len >= (ssize_t) method_len &&
			pg_strncasecmp(buf, http_methods[i], method_len) == 0)
			return true;
	}

	if (len >= 5 && pg_strncasecmp(buf, "HTTP/", 5) == 0)
		return true;

	return false;
}

