#ifndef PGSTAR_H
#define PGSTAR_H

#include "postgres.h"
#include "lib/stringinfo.h"

/* Globals */
extern const char *PGSTAR_VERSION;
extern int pgstar_port;
extern int pgstar_log_level;
extern char *pgstar_database;
extern char *pgstar_user;

void pgstar_define_gucs(void);

/* SPI helpers */
char *pgstar_json_schemata(void);
char *pgstar_json_tables(void);

/* HTTP request/response representations */
typedef struct PgstarRequest
{
	char	method[8];
	char	path[256];
} PgstarRequest;

typedef struct PgstarResponse
{
	int			status_code;
	const char *status_text;
	const char *content_type;
	char	   *body;			/* palloc'd, may be NULL */
} PgstarResponse;

bool pgstar_parse_http_request(const char *buf, ssize_t len,
							   PgstarRequest *req);

/* Response helpers */
void pgstar_http_send_response(int client_fd, PgstarResponse *resp);
void pgstar_http_send_json(int client_fd, int status_code, const char *body);
void pgstar_http_send_404(int client_fd);
void pgstar_http_send_500(int client_fd, const char *msg);
void pgstar_http_send_html_response(int client_fd, int status_code,
									const char *status_text,
									const char *html_body);
void pgstar_http_send_html_file(int client_fd, const char *filepath);

/* Router */
void pgstar_route_request(const PgstarRequest *req, PgstarResponse *resp);

#endif /* PGSTAR_H */

