#include "postgres.h"

#include "pages_layout.h"

#include "lib/stringinfo.h"
#include "utils/jsonb.h"

#include <ctype.h>
#include <string.h>

/*
 * Build a complete HTML page from fragments and metadata.
 */
void
pgstar_ui_build_html_page(const char *page_name,
						  const char *parent_tag,
						  const char *html_fragment,
						  Jsonb *meta,
						  const char *js_names,
						  const char *css_names,
						  const char *title_override,
						  StringInfo buf)
{
	bool		use_main = (parent_tag && strcmp(parent_tag, "main") == 0);
	const char *title = title_override ? title_override : (page_name ? page_name : "pgstar");

	appendStringInfoString(buf, "<!doctype html>\n<html lang=\"en\">\n<head>\n");
	appendStringInfoString(buf, "  <meta charset=\"utf-8\">\n");
	appendStringInfo(buf, "  <title>%s</title>\n", title);

	if (meta != NULL)
		pgstar_ui_append_meta_tags(buf, meta);

	pgstar_ui_append_css_links(buf, css_names);

	appendStringInfoString(buf, "</head>\n<body>\n");

	if (use_main)
	{
		appendStringInfoString(buf, "<main>\n");
		appendStringInfoString(buf, html_fragment);
		appendStringInfoString(buf, "\n</main>\n");
	}
	else
	{
		appendStringInfoString(buf, html_fragment);
		appendStringInfoString(buf, "\n");
	}

	pgstar_ui_append_js_scripts(buf, js_names);

	appendStringInfoString(buf, "\n</body>\n</html>\n");
}

/*
 * Trim whitespace from the beginning and end of a string.
 */
void
pgstar_ui_trim(char *str)
{
	char   *start = str;
	char   *end;

	if (str == NULL)
		return;

	while (*start && isspace((unsigned char) *start))
		start++;

	if (start != str)
		memmove(str, start, strlen(start) + 1);

	end = str + strlen(str);
	while (end > str && isspace((unsigned char) *(end - 1)))
		end--;

	*end = '\0';
}

