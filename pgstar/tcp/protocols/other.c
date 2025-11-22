#include "postgres.h"

#include "tcp/protocols/other.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void
pgstar_other_protocol_handle(int client_fd, const char *peek_buf, ssize_t peek_len)
{
	static const char *message = "pgstar other protocol\n";

	elog(DEBUG1, "pgstar: routed non-http request (%zd bytes)", peek_len);

	(void) send(client_fd, message, strlen(message), 0);
	close(client_fd);
}

