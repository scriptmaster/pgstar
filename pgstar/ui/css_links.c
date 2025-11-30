#include "postgres.h"

#include "pgstar.h"
#include "pages_layout.h"

#include "lib/stringinfo.h"

#include <string.h>

/*
 * Append CSS link tags to HTML head section based on comma-separated CSS names.
 * If css_names is empty, includes default CDN links for pico.css and 7.css.
 */
void
pgstar_ui_append_css_links(StringInfo head, const char *css_names)
{
	char	   *work;
	char	   *token;
	char	   *saveptr = NULL;

	if (css_names == NULL || css_names[0] == '\0')
	{
		/* Default CDN CSS links when none specified */

		// appendStringInfoString(head,
		// 	"  <link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/@picocss/pico@2.1.1/css/pico.classless.min.css\">\n");

		// emerald-v2.css
		appendStringInfoString(head,
							 "  <link rel=\"stylesheet\" href=\"https://emeraldcss-deno.scriptmaster.deno.net/emerald-v2.css\">\n");
		// 7.css
		appendStringInfoString(head,
							 "  <link rel=\"stylesheet\" href=\"https://unpkg.com/7.css@0.21.1/dist/7.css\">\n");
		return;
	}

	work = pstrdup(css_names);
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
			appendStringInfo(head,
							 "  <link rel=\"stylesheet\" href=\"%s\">\n",
							 token);
		}
		else
		{
			/* Relative path - prepend /static/css/ */
			appendStringInfo(head,
							 "  <link rel=\"stylesheet\" href=\"/static/css/%s\">\n",
							 token);
		}
	}

	pgstar_safe_pfree(work);
}

