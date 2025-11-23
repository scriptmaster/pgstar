#ifndef PGSTAR_HTTP_REQUEST_H
#define PGSTAR_HTTP_REQUEST_H

#include "pgstar.h"

void pgstar_http_request_handle(int client_fd, const char *peek_buf, ssize_t peek_len);

#endif /* PGSTAR_HTTP_REQUEST_H */

