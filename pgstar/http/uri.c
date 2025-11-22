#include "postgres.h"

#include "pgstar.h"

#include <string.h>

bool
pgstar_parse_uri(const char *uri, PgstarRequest *req)
{
	const char *qmark;
	const char *hash;
	size_t		len;

	if (uri == NULL || req == NULL)
		return false;

	memset(req->path, 0, sizeof(req->path));
	memset(req->query, 0, sizeof(req->query));
	memset(req->hash, 0, sizeof(req->hash));

	/* Copy raw URI into request buffer */
	len = strlen(uri);
	if (len >= sizeof(req->uri))
		len = sizeof(req->uri) - 1;
	memcpy(req->uri, uri, len);
	req->uri[len] = '\0';

	/* Find fragment and query boundaries in the raw URI */
	qmark = strchr(req->uri, '?');
	hash = strchr(req->uri, '#');

	/* Path ends before '?' or '#' (whichever comes first) */
	if (qmark && (!hash || qmark < hash))
		len = qmark - req->uri;
	else if (hash)
		len = hash - req->uri;
	else
		len = strlen(req->uri);

	if (len >= sizeof(req->path))
		len = sizeof(req->path) - 1;
	memcpy(req->path, req->uri, len);
	req->path[len] = '\0';

	/* Query: between '?' and '#' (or end) */
	if (qmark)
	{
		const char *qstart = qmark + 1;
		const char *qend = hash ? hash : (req->uri + strlen(req->uri));
		size_t		qlen = (qend > qstart) ? (size_t) (qend - qstart) : 0;

		if (qlen >= sizeof(req->query))
			qlen = sizeof(req->query) - 1;
		memcpy(req->query, qstart, qlen);
		req->query[qlen] = '\0';
	}

	/* Fragment: after '#' */
	if (hash)
	{
		const char *hstart = hash + 1;
		size_t		hlen = strlen(hstart);

		if (hlen >= sizeof(req->hash))
			hlen = sizeof(req->hash) - 1;
		memcpy(req->hash, hstart, hlen);
		req->hash[hlen] = '\0';
	}

	return true;
}

int
pgstar_split_path_segments(const char *path, char segments[][64], int max_segments)
{
	const char *p = path;
	int			count = 0;

	if (p == NULL)
		return 0;

	/* Skip leading '/' if present */
	if (*p == '/')
		p++;

	while (*p != '\0' && count < max_segments)
	{
		const char *slash = strchr(p, '/');
		size_t		len;

		if (slash)
			len = (size_t) (slash - p);
		else
			len = strlen(p);

		if (len >= sizeof(segments[0]))
			len = sizeof(segments[0]) - 1;

		memcpy(segments[count], p, len);
		segments[count][len] = '\0';
		count++;

		if (!slash)
			break;
		p = slash + 1;
	}

	return count;
}

bool
pgstar_get_query_param(const char *query, const char *key,
					   char *value, size_t value_len)
{
	const char *p;
	size_t		keylen;

	if (query == NULL || key == NULL || value == NULL || value_len == 0)
		return false;

	if (*query == '\0' || *key == '\0')
		return false;

	keylen = strlen(key);
	p = query;

	while (*p != '\0')
	{
		const char *amp = strchr(p, '&');
		const char *eq = strchr(p, '=');
		const char *end;

		if (eq == NULL || (amp && amp < eq))
		{
			/* No '=' in this segment; skip to next */
			if (!amp)
				break;
			p = amp + 1;
			continue;
		}

		if ((size_t) (eq - p) == keylen &&
			strncmp(p, key, keylen) == 0)
		{
			const char *vstart = eq + 1;

			end = amp ? amp : (query + strlen(query));

			if (end > vstart)
			{
				size_t	len = (size_t) (end - vstart);

				if (len >= value_len)
					len = value_len - 1;
				memcpy(value, vstart, len);
				value[len] = '\0';
				return true;
			}

			/* key present but empty value */
			value[0] = '\0';
			return true;
		}

		if (!amp)
			break;
		p = amp + 1;
	}

	return false;
}


