/*
 * pgstar/http/client.c
 * Simple HTTPS client using OpenSSL.
 */

#include "postgres.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>

#include "pgstar.h"

/* 
 * Helper to parse URL into host, port, path 
 * Supports http:// and https://
 */
static bool
parse_url(const char *url, char *host, int host_len, int *port, char *path, int path_len, bool *is_ssl)
{
	const char *p = url;
	const char *host_start;
	int h_len;
	
	*is_ssl = false;
	*port = 80;

	if (strncmp(p, "http://", 7) == 0)
	{
		p += 7;
		*port = 80;
	}
	else if (strncmp(p, "https://", 8) == 0)
	{
		p += 8;
		*is_ssl = true;
		*port = 443;
	}
	else
	{
		/* Default to http if no scheme? Or fail? Let's fail or assume http */
		/* User asked for https download specifically, but let's support both */
		return false; 
	}

	host_start = p;
	
	/* Find end of host (slash or colon) */
	while (*p && *p != '/' && *p != ':')
		p++;
	
	h_len = p - host_start;
	if (h_len >= host_len) return false;
	
	memcpy(host, host_start, h_len);
	host[h_len] = '\0';

	if (*p == ':')
	{
		p++;
		*port = atoi(p);
		while (*p && *p != '/')
			p++;
	}

	if (*p == '\0')
	{
		if (path_len > 1) strcpy(path, "/");
	}
	else
	{
		if ((int)strlen(p) >= path_len) return false;
		strcpy(path, p);
	}

	return true;
}

/*
 * Downloads data from a URL.
 * Returns palloc'd buffer and sets *len.
 * Returns NULL on failure.
 */
char *
pgstar_http_download(const char *url, size_t *len, char **content_type)
{
	char host[256];
	char path[1024];
	int port;
	bool is_ssl;
	int sock;
	struct hostent *server;
	struct sockaddr_in server_addr;
	SSL_CTX *ctx = NULL;
	SSL *ssl = NULL;
	char request[2048];
	char *response = NULL;
	size_t response_size = 0;
	size_t total_read = 0;
	char buffer[4096];
	int bytes;
	char *body_start = NULL;
	int header_len = 0;

	*len = 0;
	if (content_type) *content_type = NULL;

	elog(pgstar_log_level, "pgstar: http_download start: %s", url);

	if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path), &is_ssl))
	{
		elog(WARNING, "pgstar: invalid url: %s", url);
		return NULL;
	}

	/* DNS Lookup */
	server = gethostbyname(host);
	if (server == NULL)
	{
		elog(WARNING, "pgstar: no such host: %s", host);
		return NULL;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		elog(WARNING, "pgstar: error opening socket");
		return NULL;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
	server_addr.sin_port = htons(port);

	if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		elog(WARNING, "pgstar: error connecting to %s:%d", host, port);
		close(sock);
		return NULL;
	}

	if (is_ssl)
	{
		/* Init OpenSSL */
		/* Note: In a Postgres extension, we should assume OpenSSL might be init'd.
		 * We create a context for this connection. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
		SSL_library_init();
		SSL_load_error_strings();
#endif
		const SSL_METHOD *method = TLS_client_method();
		ctx = SSL_CTX_new(method);
		if (!ctx)
		{
			elog(WARNING, "pgstar: SSL_CTX_new failed");
			close(sock);
			return NULL;
		}

		ssl = SSL_new(ctx);
		SSL_set_fd(ssl, sock);
		/* SNI */
		SSL_set_tlsext_host_name(ssl, host);

		if (SSL_connect(ssl) <= 0)
		{
			elog(WARNING, "pgstar: SSL_connect failed");
			ERR_print_errors_fp(stderr);
			SSL_free(ssl);
			SSL_CTX_free(ctx);
			close(sock);
			return NULL;
		}
	}

	/* Send GET request */
	snprintf(request, sizeof(request), 
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: pgstar/1.0\r\n"
		"Connection: close\r\n"
		"\r\n", 
		path, host);

	if (is_ssl)
		SSL_write(ssl, request, strlen(request));
	else
		write(sock, request, strlen(request));

	/* Read Response */
	/* Simple implementation: read everything into a growing buffer */
	response = (char *) palloc(4096);
	response_size = 4096;

	while (1)
	{
		if (total_read + 4096 > response_size)
		{
			response_size *= 2;
			response = (char *) repalloc(response, response_size);
		}

		if (is_ssl)
			bytes = SSL_read(ssl, response + total_read, 4096);
		else
			bytes = read(sock, response + total_read, 4096);

		if (bytes <= 0) break;
		total_read += bytes;
	}

	if (is_ssl)
	{
		/* SSL_shutdown(ssl); -- Can be noisy or blocking, often just freeing is fine for client */
		SSL_free(ssl);
		SSL_CTX_free(ctx);
	}
	close(sock);

	/* Parse Headers */
	/* Find double CRLF */
	char *eoh = NULL;
	int i;
	/* Be careful not to overrun buffer or miss CRLF split across reads (though we read fully first) */
	/* We search for \r\n\r\n */
	for (i = 0; i < (int)total_read - 3; i++)
	{
		if (response[i] == '\r' && response[i+1] == '\n' && 
			response[i+2] == '\r' && response[i+3] == '\n')
		{
			eoh = response + i;
			header_len = i + 4;
			body_start = response + header_len;
			break;
		}
	}

	if (!body_start)
	{
		elog(WARNING, "pgstar: invalid HTTP response (no headers)");
		pfree(response);
		return NULL;
	}

	/* Check 200 OK */
	if (strncmp(response, "HTTP/1.", 7) == 0)
	{
		char *status = strchr(response, ' ');
		if (status && atoi(status + 1) != 200)
		{
			elog(WARNING, "pgstar: HTTP error status: %.3s", status + 1);
			/* We could return the body as error text, but for now NULL */
			pfree(response);
			return NULL;
		}
	}

	/* Handle Transfer-Encoding: chunked */
	bool is_chunked = false;
	{
		char *te_header = strstr(response, "Transfer-Encoding:");
		if (te_header && te_header < eoh)
		{
			if (strstr(te_header, "chunked") != NULL) /* sloppy check but likely sufficient */
				is_chunked = true;
		}
	}

	/* Extract Content-Type if requested */
	if (content_type)
	{
		char *ct_header = strstr(response, "Content-Type:");
		if (ct_header && ct_header < eoh)
		{
			ct_header += 13;
			while (*ct_header == ' ') ct_header++;
			char *ct_end = strstr(ct_header, "\r\n");
			if (ct_end)
			{
				int ct_len = ct_end - ct_header;
				*content_type = palloc(ct_len + 1);
				memcpy(*content_type, ct_header, ct_len);
				(*content_type)[ct_len] = '\0';
			}
		}
	}

	/* Copy body to new buffer to trim/finalize */
	if (is_chunked)
	{
		elog(pgstar_log_level, "pgstar: decoding chunked response");
		
		/* Decode chunked body */
		size_t body_raw_len = total_read - header_len;
		char *decoded = (char *) palloc(body_raw_len); /* Decoded is always smaller */
		size_t decoded_len = 0;
		char *ptr = body_start;
		char *end = response + total_read;
		
		while (ptr < end)
		{
			/* Parse chunk size (hex) */
			char *chunk_data_start = strstr(ptr, "\r\n");
			if (!chunk_data_start) break;
			
			long chunk_size = strtol(ptr, NULL, 16);
			if (chunk_size == 0) break; /* Last chunk */
			
			ptr = chunk_data_start + 2; /* Skip \r\n after size */
			
			if (ptr + chunk_size > end)
			{
				elog(WARNING, "pgstar: chunk size exceeds buffer");
				break;
			}
			
			memcpy(decoded + decoded_len, ptr, chunk_size);
			decoded_len += chunk_size;
			
			ptr += chunk_size + 2; /* Skip data + \r\n */
		}
		
		*len = decoded_len;
		pfree(response);
		return decoded;
	}
	else
	{
		*len = total_read - header_len;
		char *final_body = (char *) palloc(*len);
		memcpy(final_body, body_start, *len);
		pfree(response);
		return final_body;
	}
}

