#include "postgres.h"

#include "pages_layout.h"

#include "lib/stringinfo.h"

#include <string.h>

/*
 * Append JavaScript script tags to HTML body section based on comma-separated JS names.
 * If js_names is empty, includes default datastar CDN script.
 */
void
pgstar_ui_append_js_scripts(StringInfo body, const char *js_names)
{
	char	   *work;
	char	   *token;
	char	   *saveptr = NULL;

	if (js_names == NULL || js_names[0] == '\0')
	{
		/* Default datastar CDN script when none specified */
		appendStringInfoString(body,
							 "  <script src=\"https://cdn.jsdelivr.net/gh/starfederation/datastar@1.0.0-RC.6/bundles/datastar.js\"></script>\n");
		return;
	}

	work = pstrdup(js_names);
	pgstar_ui_trim(work);

	for (token = strtok_r(work, ",", &saveptr);
		 token != NULL;
		 token = strtok_r(NULL, ",", &saveptr))
	{
		pgstar_ui_trim(token);
		if (token[0] == '\0')
			continue;

		/* Check if it's a full URL (http:// or https://) */
		if (strncmp(token, "http://", 7) == 0 || strncmp(token, "https://", 8) == 0)
		{
			appendStringInfo(body,
							 "  <script src=\"%s\"></script>\n",
							 token);
		}
		else
		{
			/* Relative path - prepend /static/js/ */
			appendStringInfo(body,
							 "  <script src=\"/static/js/%s\"></script>\n",
							 token);
		}
	}

	pfree(work);
}

