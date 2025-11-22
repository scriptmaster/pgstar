#ifndef PGSTAR_TCP_PROTOCOLS_OTHER_H
#define PGSTAR_TCP_PROTOCOLS_OTHER_H

#include "postgres.h"

void pgstar_other_protocol_handle(int client_fd, const char *peek_buf, ssize_t peek_len);

#endif /* PGSTAR_TCP_PROTOCOLS_OTHER_H */

