#ifndef PGSTAR_H
#define PGSTAR_H

#include "postgres.h"
#include "lib/stringinfo.h"
#include "core/env.h"

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
char *pgstar_json_files(void);

/* HTTP request/response representations */
typedef struct PgstarRequest
{
	char	method[8];
	char	uri[256];		/* raw path+query+fragment */
	char	path[256];		/* path without query/fragment */
	char	query[256];		/* portion after '?' (no '#') */
	char	hash[128];		/* fragment (after '#') */
	const char *body;		/* request body (points into request buffer) */
	size_t	body_len;		/* request body length */
} PgstarRequest;

typedef struct PgstarResponse
{
	int			status_code;
	const char *status_text;
	const char *content_type;
	char	   *body;			/* palloc'd, may be NULL (text data) */
	size_t		body_len;		/* body length (for binary data, use this instead of strlen) */
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
void pgstar_route_v1(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_v1_chat(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_v1_ui_models(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_v1_models_convert(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_v1_models_local(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ai(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_static(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_pgstar(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_pages(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_js(const PgstarRequest *req, PgstarResponse *resp);
void pgstar_route_ui_css(const PgstarRequest *req, PgstarResponse *resp);

/* UI setup */
void pgstar_ui_ensure_pages(void);
bool pgstar_ui_fetch_file(const char *file_name, char **body, size_t *body_len, const char **content_type);
void pgstar_route_ui_favicon(const PgstarRequest *req, PgstarResponse *resp);
extern const char *pgstar_root_html;

/* HTTP Client */
char *pgstar_http_download(const char *url, size_t *len, char **content_type);

/* Safe memory / pointer tracking utilities */
void pgstar_safe_pfree(void *ptr);
void pgstar_ptr_track(const char *label, const void *ptr, ssize_t delta, Size bytes);

#define PGSTAR_VERBOSE_LOG(fmt, ...) \
	do { \
		if (pgstar_env_verbose()) \
			elog(pgstar_log_level, fmt, ##__VA_ARGS__); \
	} while (0)

#define PGSTAR_TRACK_ALLOC(label, ptr, bytes) \
	do { \
		const void *__pgstar_ptr = (const void *) (ptr); \
		Size __pgstar_bytes = (Size) (bytes); \
		pgstar_ptr_track((label), __pgstar_ptr, +1, __pgstar_bytes); \
	} while (0)

#define PGSTAR_TRACK_FREE(label, ptr) \
	do { \
		const void *__pgstar_ptr = (const void *) (ptr); \
		pgstar_ptr_track((label), __pgstar_ptr, -1, 0); \
	} while (0)

#endif /* PGSTAR_H */

