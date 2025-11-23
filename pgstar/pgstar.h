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
extern int pgstar_ui_cache_ttl;

void pgstar_define_gucs(void);

/* SPI helpers */
char *pgstar_json_schemata(void);
char *pgstar_json_tables(void);

/* HTTP request/response representations */
typedef struct PgstarRequest
{
	char	method[8];
	char	uri[256];		/* raw path+query+fragment */
	char	path[256];		/* path without query/fragment */
	char	query[256];		/* portion after '?' (no '#') */
	char	hash[128];		/* fragment (after '#') */
} PgstarRequest;

typedef struct PgstarResponse
{
	int			status_code;
	const char *status_text;
	const char *content_type;
	char	   *body;			/* palloc'd, may be NULL */
	char	   *log;			/* optional X-Log header value */
} PgstarResponse;

bool pgstar_parse_http_request(const char *buf, ssize_t len,
							   PgstarRequest *req);
bool pgstar_parse_uri(const char *uri, PgstarRequest *req);
int  pgstar_split_path_segments(const char *path, char segments[][64], int max_segments);
bool pgstar_get_query_param(const char *query, const char *key,
							char *value, size_t value_len);

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
void pgstar_route_api(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ai(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_static(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_pgstar(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_pages(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_js(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_css(const PgstarRequest *req, PgstarResponse *resp);

/* UI setup */
void pgstar_ui_ensure_pages(void);
extern const char *pgstar_root_html;

/* Safe memory utilities */
void pgstar_safe_pfree(void *ptr);

#endif /* PGSTAR_H */

