#include "postgres.h"
#include "fmgr.h"

#include "utils/builtins.h"
#include "utils/jsonb.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pgstar.h"

PG_FUNCTION_INFO_V1(pgstar_http_post);

static void pgstar_append_headers_from_jsonb(StringInfo buf, Jsonb *jb);

Datum
pgstar_http_post(PG_FUNCTION_ARGS)
{
	text	   *url_text;
	Jsonb	   *headers = NULL;
	text	   *body_text = NULL;
	char	   *url;
	char	   *body = NULL;
	char		host[256];
	char		path[512];
	char		portstr[6];
	int			port = 80;
	const char *p;
	const char *host_start;
	const char *host_end;
	const char *path_start;
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int			sockfd = -1;
	StringInfoData reqbuf;
	StringInfoData respbuf;
	char		tmp[4096];
	ssize_t		nread;
	text	   *result_text = NULL;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errmsg("pgstar_http_post: url must not be NULL")));

	url_text = PG_GETARG_TEXT_PP(0);
	url = text_to_cstring(url_text);

	if (!PG_ARGISNULL(1))
		headers = PG_GETARG_JSONB_P(1);

	if (!PG_ARGISNULL(2))
	{
		body_text = PG_GETARG_TEXT_PP(2);
		body = text_to_cstring(body_text);
	}
	else
	{
		body = pstrdup("");
	}

	/* Very simple URL parser: support only http://host[:port]/path */
	p = url;
	if (pg_strncasecmp(p, "http://", 7) == 0)
		p += 7;
	else if (pg_strncasecmp(p, "https://", 8) == 0)
		ereport(ERROR,
				(errmsg("pgstar_http_post: https URLs are not supported, use http://")));

	host_start = p;
	host_end = strchr(p, '/');
	if (host_end == NULL)
	{
		/* No explicit path; default to root */
		path_start = "/";
		host_end = url + strlen(url);
	}
	else
	{
		path_start = host_end;
	}

	if ((host_end - host_start) <= 0 || (host_end - host_start) >= (int) sizeof(host))
		ereport(ERROR,
				(errmsg("pgstar_http_post: invalid or too long host in URL")));

	memcpy(host, host_start, host_end - host_start);
	host[host_end - host_start] = '\0';

	/* Extract port if present in host header */
	{
		char   *colon = strchr(host, ':');

		if (colon)
		{
			long	tmp_port;

			*colon = '\0';
			tmp_port = strtol(colon + 1, NULL, 10);
			if (tmp_port <= 0 || tmp_port > 65535)
				ereport(ERROR,
						(errmsg("pgstar_http_post: invalid port in URL")));
			port = (int) tmp_port;
		}
	}

	if (strlen(path_start) >= sizeof(path))
		ereport(ERROR,
				(errmsg("pgstar_http_post: path in URL is too long")));
	strlcpy(path, path_start, sizeof(path));

	snprintf(portstr, sizeof(portstr), "%d", port);

	initStringInfo(&reqbuf);
	appendStringInfo(&reqbuf, "POST %s HTTP/1.1\r\n", path);
	appendStringInfo(&reqbuf, "Host: %s\r\n", host);
	appendStringInfoString(&reqbuf, "Connection: close\r\n");

	if (headers != NULL)
		pgstar_append_headers_from_jsonb(&reqbuf, headers);

	appendStringInfo(&reqbuf, "Content-Length: %zu\r\n", strlen(body));
	appendStringInfoString(&reqbuf, "\r\n");
	appendBinaryStringInfo(&reqbuf, body, strlen(body));

	MemSet(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	PG_TRY();
	{
		int			rc;

		rc = getaddrinfo(host, portstr, &hints, &res);
		if (rc != 0 || res == NULL)
			ereport(ERROR,
					(errmsg("pgstar_http_post: getaddrinfo failed for host \"%s\": %s",
							host, gai_strerror(rc))));

		for (rp = res; rp != NULL; rp = rp->ai_next)
		{
			sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (sockfd < 0)
				continue;

			if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
				break;

			close(sockfd);
			sockfd = -1;
		}

		if (sockfd < 0)
			ereport(ERROR,
					(errmsg("pgstar_http_post: could not connect to %s:%s", host, portstr)));

		/* Send full request */
		{
			ssize_t		sent = 0;
			ssize_t		len = reqbuf.len;

			while (sent < len)
			{
				ssize_t		n = send(sockfd, reqbuf.data + sent, len - sent, 0);

				if (n < 0)
				{
					if (errno == EINTR)
						continue;
					ereport(ERROR,
							(errmsg("pgstar_http_post: send() failed: %m")));
				}
				if (n == 0)
					break;
				sent += n;
			}
		}

		/* Read response */
		initStringInfo(&respbuf);
		for (;;)
		{
			nread = recv(sockfd, tmp, sizeof(tmp), 0);
			if (nread < 0)
			{
				if (errno == EINTR)
					continue;
				ereport(ERROR,
						(errmsg("pgstar_http_post: recv() failed: %m")));
			}
			if (nread == 0)
				break;
			appendBinaryStringInfo(&respbuf, tmp, nread);
		}

		/* Strip HTTP headers, return only body */
		{
			char   *body_start;

			body_start = strstr(respbuf.data, "\r\n\r\n");
			if (body_start != NULL)
				body_start += 4;
			else
				body_start = respbuf.data;

			result_text = cstring_to_text(body_start);
		}
	}
	PG_CATCH();
	{
		if (sockfd >= 0)
		{
			close(sockfd);
			sockfd = -1;
		}
		if (res != NULL)
		{
			freeaddrinfo(res);
			res = NULL;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (sockfd >= 0)
		close(sockfd);
	if (res != NULL)
		freeaddrinfo(res);

	if (result_text == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(result_text);
}

static void
pgstar_append_headers_from_jsonb(StringInfo buf, Jsonb *jb)
{
	JsonbIterator *it;
	JsonbValue	v;
	int			r;
	char	   *current_key = NULL;

	if (jb == NULL)
		return;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (r == WJB_KEY && v.type == jbvString)
		{
			if (current_key)
			{
				pfree(current_key);
				current_key = NULL;
			}

			current_key = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (r == WJB_VALUE && current_key != NULL)
		{
			if (v.type == jbvString)
			{
				appendStringInfo(buf, "%s: %.*s\r\n",
								 current_key,
								 v.val.string.len,
								 v.val.string.val);
			}

			pfree(current_key);
			current_key = NULL;
		}
	}

	if (current_key)
		pfree(current_key);
}


