#ifndef PGSTAR_UI_PAGES_LAYOUT_H
#define PGSTAR_UI_PAGES_LAYOUT_H

#include "postgres.h"
#include "lib/stringinfo.h"
#include "utils/jsonb.h"

/* HTML page building functions */
void pgstar_ui_build_html_page(const char *page_name,
							  const char *parent_tag,
							  const char *html_fragment,
							  Jsonb *meta,
							  const char *js_names,
							  const char *css_names,
							  StringInfo buf);

void pgstar_ui_append_meta_tags(StringInfo head, Jsonb *meta);
void pgstar_ui_append_css_links(StringInfo head, const char *css_names);
void pgstar_ui_append_js_scripts(StringInfo body, const char *js_names);
void pgstar_ui_trim(char *str);

#endif /* PGSTAR_UI_PAGES_LAYOUT_H */

