#include "postgres.h"

#include "pgstar.h"
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
		/* Default Vue.js when none specified */
		appendStringInfoString(body,
							 "  <script src=\"https://cdnjs.cloudflare.com/ajax/libs/vue/3.5.22/vue.global.prod.min.js\"></script>\n");
		appendStringInfoString(body,
							 "  <script src=\"/static/js/vue-data-table.js\"></script>\n");
		return;
	}

	work = pstrdup(js_names);
	pgstar_ui_trim(work);

	for (token = strtok_r(work, ",", &saveptr);
		 token != NULL;
		 token = strtok_r(NULL, ",", &saveptr))
	{
		const char *type_attr;
		const char *prefix;

		pgstar_ui_trim(token);
		if (token[0] == '\0')
			continue;

		/* Check if token contains "module" keyword */
		type_attr = (strstr(token, "module") != NULL) ? "type=\"module\"" : "";

		/* Determine prefix: empty for full URLs, /static/js/ for relative paths */
		prefix = "";
		if (strncmp(token, "http://", 7) != 0 && strncmp(token, "https://", 8) != 0)
		{
			prefix = "/static/js/";
		}

		appendStringInfo(body,
						 "  <script %s src=\"%s%s\"></script>\n",
						 type_attr, prefix, token);
	}

	pgstar_safe_pfree(work);
}

