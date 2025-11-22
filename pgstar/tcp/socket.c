#include "postgres.h"

#include "tcp/socket.h"

#include "tcp/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int
pgstar_tcp_open_socket(int port)
{
	int			sockfd;
	struct sockaddr_in addr;
	int			optval = 1;
	int			flags;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		ereport(ERROR,
				(errmsg("pgstar: could not create socket: %m")));

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
		ereport(WARNING,
				(errmsg("pgstar: setsockopt(SO_REUSEADDR) failed: %m")));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		ereport(ERROR,
				(errmsg("pgstar: could not bind to port %d: %m", port)));

	if (listen(sockfd, SOMAXCONN) < 0)
		ereport(ERROR,
				(errmsg("pgstar: listen() failed on port %d: %m", port)));

	flags = fcntl(sockfd, F_GETFL, 0);
	if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
		ereport(ERROR,
				(errmsg("pgstar: could not set non-blocking mode: %m")));

	elog(LOG, "pgstar: listening on port %d", port);
	return sockfd;
}

void
pgstar_tcp_accept_loop(int sockfd)
{
	for (;;)
	{
		struct sockaddr_in client_addr;
		socklen_t	client_len = sizeof(client_addr);
		int			client_fd;
		int			cflags;

		client_fd = accept(sockfd, (struct sockaddr *) &client_addr, &client_len);
		if (client_fd < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			ereport(WARNING,
					(errmsg("pgstar: accept() failed: %m")));
			break;
		}

		cflags = fcntl(client_fd, F_GETFL, 0);
		if (cflags >= 0)
			(void) fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);

		pgstar_tcp_protocol_route(client_fd);
	}
}

