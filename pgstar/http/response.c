#include "postgres.h"

#include "core/pgstar.h"

#include "lib/stringinfo.h"
#include "storage/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>

void
pgstar_http_send_response(int client_fd, PgstarResponse *resp)
{
    char    header[512];
    size_t  body_len = resp->body ? strlen(resp->body) : 0;
    struct iovec iov[2];
    int     iovcnt;
    ssize_t written;

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     resp->status_code,
                     resp->status_text ? resp->status_text : "",
                     resp->content_type ? resp->content_type : "text/plain",
                     body_len);

    if (n <= 0)
        return;

    iov[0].iov_base = header;
    iov[0].iov_len  = (size_t)n;
    iov[1].iov_base = resp->body;
    iov[1].iov_len  = body_len;

    iovcnt = (resp->body ? 2 : 1);
    written = writev(client_fd, iov, iovcnt);
    (void) written;
}

void
pgstar_http_send_json(int client_fd, int status_code,
                      const char *body)
{
    PgstarResponse resp;

    memset(&resp, 0, sizeof(resp));
    resp.status_code  = status_code;
    resp.status_text  = (status_code == 200) ? "OK" :
                        (status_code == 404) ? "Not Found" :
                        (status_code == 500) ? "Internal Server Error" :
                        "OK";
    resp.content_type = "application/json";
    resp.body         = body ? pstrdup(body) : NULL; /* own the buffer */

    pgstar_http_send_response(client_fd, &resp);
    if (resp.body)
        pfree(resp.body);
}

void
pgstar_http_send_404(int client_fd)
{
    char *body = "{\"error\":\"not found\"}\n";
    pgstar_http_send_json(client_fd, 404, body);
}

void
pgstar_http_send_500(int client_fd, const char *msg)
{
    StringInfoData buf;
    PgstarResponse resp;

    initStringInfo(&buf);
    appendStringInfo(&buf,
                     "{\"error\":\"internal\",\"detail\":\"%s\"}\n",
                     msg ? msg : "unknown");

    memset(&resp, 0, sizeof(resp));
    resp.status_code  = 500;
    resp.status_text  = "Internal Server Error";
    resp.content_type = "application/json";
    resp.body         = buf.data;

    pgstar_http_send_response(client_fd, &resp);
    pfree(buf.data);
}

void
pgstar_http_send_html_response(int client_fd, int status_code,
                               const char *status_text,
                               const char *html_body)
{
    PgstarResponse resp;

    memset(&resp, 0, sizeof(resp));
    resp.status_code  = status_code;
    resp.status_text  = status_text ? status_text : "OK";
    resp.content_type = "text/html; charset=utf-8";
    resp.body         = html_body ? pstrdup(html_body) : NULL;

    pgstar_http_send_response(client_fd, &resp);
    if (resp.body)
        pfree(resp.body);
}

void
pgstar_http_send_html_file(int client_fd, const char *filepath)
{
    FILE       *file;
    StringInfoData buf;
    char        chunk[1024];
    size_t      nread;

    file = AllocateFile(filepath, "rb");
    if (file == NULL)
    {
        pgstar_http_send_404(client_fd);
        return;
    }

    initStringInfo(&buf);
    while ((nread = fread(chunk, 1, sizeof(chunk), file)) > 0)
        appendBinaryStringInfo(&buf, chunk, (int) nread);

    FreeFile(file);

    pgstar_http_send_html_response(client_fd, 200, "OK", buf.data);
    pfree(buf.data);
}
