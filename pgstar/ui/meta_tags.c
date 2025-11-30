#include "postgres.h"

#include "pgstar.h"
#include "pages_layout.h"

#include "lib/stringinfo.h"
#include "utils/jsonb.h"

#include <string.h>

/*
 * Append meta tags to HTML head section based on JSONB metadata.
 */
void
pgstar_ui_append_meta_tags(StringInfo head, Jsonb *meta)
{
	JsonbIterator *it;
	JsonbValue	v;
	int			r;
	bool		in_object = false;
	char	   *current_key = NULL;
	char	   *http_equiv = NULL;
	char	   *name = NULL;
	char	   *content = NULL;

	if (meta == NULL)
		return;

	it = JsonbIteratorInit(&meta->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (r == WJB_BEGIN_OBJECT)
		{
			in_object = true;
			if (http_equiv)
				pgstar_safe_pfree(http_equiv);
			if (name)
				pgstar_safe_pfree(name);
			if (content)
				pgstar_safe_pfree(content);
			http_equiv = name = content = NULL;
		}
		else if (r == WJB_END_OBJECT)
		{
			if (in_object)
			{
				if (http_equiv && content)
				{
					appendStringInfo(head,
									 "  <meta http-equiv=\"%s\" content=\"%s\">\n",
									 http_equiv,
									 content);
				}
				else if (name && content)
				{
					appendStringInfo(head,
									 "  <meta name=\"%s\" content=\"%s\">\n",
									 name,
									 content);
				}
			}
			in_object = false;
		}
		else if (r == WJB_KEY && v.type == jbvString)
		{
			if (current_key)
			{
					pgstar_safe_pfree(current_key);
				current_key = NULL;
			}
			current_key = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (r == WJB_VALUE && current_key != NULL)
		{
			if (v.type == jbvString)
			{
				char   *val = pnstrdup(v.val.string.val, v.val.string.len);

				if (pg_strcasecmp(current_key, "http-equiv") == 0)
				{
					if (http_equiv)
						pgstar_safe_pfree(http_equiv);
					http_equiv = val;
				}
				else if (pg_strcasecmp(current_key, "name") == 0)
				{
					if (name)
						pgstar_safe_pfree(name);
					name = val;
				}
				else if (pg_strcasecmp(current_key, "content") == 0)
				{
					if (content)
						pgstar_safe_pfree(content);
					content = val;
				}
				else
				{
					pgstar_safe_pfree(val);
				}
			}

					pgstar_safe_pfree(current_key);
			current_key = NULL;
		}
	}

	if (current_key)
					pgstar_safe_pfree(current_key);
	if (http_equiv)
		pfree(http_equiv);
	if (name)
		pfree(name);
	if (content)
		pfree(content);
}

