#ifndef PGSTAR_TCP_SOCKET_H
#define PGSTAR_TCP_SOCKET_H

int pgstar_tcp_open_socket(int port);
void pgstar_tcp_accept_loop(int sockfd);

#endif /* PGSTAR_TCP_SOCKET_H */

