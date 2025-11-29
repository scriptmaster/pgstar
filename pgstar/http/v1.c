#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>

extern void pgstar_route_v1_chat(const PgstarRequest *req, PgstarResponse *resp);
extern void pgstar_route_v1_ui_models(const PgstarRequest *req, PgstarResponse *resp);
extern void pgstar_route_v1_models_convert(const PgstarRequest *req, PgstarResponse *resp);
extern void pgstar_route_v1_models_local(const PgstarRequest *req, PgstarResponse *resp);

void
pgstar_route_v1(const PgstarRequest *req, PgstarResponse *resp)
{
	char		segs_buf[4][64];
	const char *segs[4];
	int			nseg;
	int			i;

	nseg = pgstar_split_path_segments(req->path, segs_buf, 4);
	for (i = 0; i < nseg; i++)
		segs[i] = segs_buf[i];

	/* Expect at least /v1/... */
	if (nseg < 1 || strcmp(segs[0], "v1") != 0)
	{
		resp->status_code = 404;
		resp->status_text = "Not Found";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"invalid v1 path\"}\n");
		return;
	}

	/* /v1/chat/completions */
	if (nseg >= 2 && strcmp(segs[1], "chat") == 0)
	{
		if (nseg >= 3 && strcmp(segs[2], "completions") == 0)
		{
			pgstar_route_v1_chat(req, resp);
			return;
		}
	}

	/* /v1/ui/models */
	if (nseg >= 2 && strcmp(segs[1], "ui") == 0)
	{
		if (nseg >= 3 && strcmp(segs[2], "models") == 0)
		{
			pgstar_route_v1_ui_models(req, resp);
			return;
		}
	}

	/* /v1/models/convert */
	if (nseg >= 2 && strcmp(segs[1], "models") == 0)
	{
		if (nseg >= 3 && strcmp(segs[2], "convert") == 0)
		{
			pgstar_route_v1_models_convert(req, resp);
			return;
		}
		if (nseg >= 3 && strcmp(segs[2], "local") == 0)
		{
			pgstar_route_v1_models_local(req, resp);
			return;
		}
	}

	resp->status_code = 404;
	resp->status_text = "Not Found";
	resp->content_type = "application/json";
	resp->body = pstrdup("{\"error\":\"not found\"}\n");
}


