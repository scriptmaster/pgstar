#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* Forward declarations */
static void pgstar_models_local_build_json(StringInfo buf);

void
pgstar_route_v1_models_local(const PgstarRequest *req, PgstarResponse *resp)
{
	StringInfoData json_buf;

	elog(pgstar_log_level, "pgstar: v1/models/local request: %s", req->method);

	if (pg_strcasecmp(req->method, "GET") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	/* Build JSON response with local models */
	initStringInfo(&json_buf);
	pgstar_models_local_build_json(&json_buf);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = "application/json";
	resp->body = json_buf.data;
}

static void
pgstar_models_local_build_json(StringInfo buf)
{
	DIR		   *dir;
	struct dirent *entry;
	struct stat st;
	char		filepath[512];
	char		model_name[256];
	const char *ext;
	const char *base;
	const char *model_start;
	int			first_entry = 1;

	appendStringInfoString(buf, "{\n");
	appendStringInfoString(buf, "  \"models\": [\n");

	dir = opendir("/opt/pgstar/models");
	if (dir == NULL)
	{
		/* Directory doesn't exist or can't be read */
		appendStringInfoString(buf, "  ],\n");
		appendStringInfoString(buf, "  \"error\": \"Could not read models directory\"\n");
		appendStringInfoString(buf, "}\n");
		return;
	}

	/* First pass: collect all model names */
	while ((entry = readdir(dir)) != NULL)
	{
		if (entry->d_name[0] == '.')
			continue;

		snprintf(filepath, sizeof(filepath), "/opt/pgstar/models/%s", entry->d_name);
		if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		ext = strrchr(entry->d_name, '.');
		if (ext == NULL)
			continue;

		base = entry->d_name;

		if (strcmp(ext, ".onnx") == 0)
		{
			/* Extract model name from ONNX filename */
			model_start = strstr(base, "pgstar_model_");
			if (model_start)
			{
				model_start += strlen("pgstar_model_");
				strncpy(model_name, model_start, ext - model_start);
				model_name[ext - model_start] = '\0';
			}
			else
			{
				/* Just the base name without extension */
				strncpy(model_name, base, ext - base);
				model_name[ext - base] = '\0';
			}

			/* Add ONNX model */
			if (!first_entry)
				appendStringInfoString(buf, ",\n");
			appendStringInfoString(buf, "    {\n");
			appendStringInfo(buf, "      \"name\": \"%s\",\n", model_name);
			appendStringInfoString(buf, "      \"description\": \"Local ONNX model (can convert to NCNN)\",\n");
			appendStringInfoString(buf, "      \"downloads\": 0,\n");
			appendStringInfoString(buf, "      \"source\": \"Local\",\n");
			appendStringInfoString(buf, "      \"format\": \"ONNX\",\n");
			appendStringInfo(buf, "      \"url\": \"https://huggingface.co/%s\",\n", model_name);
			appendStringInfo(buf, "      \"file\": \"%s\"\n", entry->d_name);
			appendStringInfoString(buf, "    }");
			first_entry = 0;
		}
	}

	/* Second pass: find NCNN model pairs */
	rewinddir(dir);
	while ((entry = readdir(dir)) != NULL)
	{
		if (entry->d_name[0] == '.')
			continue;

		snprintf(filepath, sizeof(filepath), "/opt/pgstar/models/%s", entry->d_name);
		if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		ext = strrchr(entry->d_name, '.');
		if (ext == NULL)
			continue;

		base = entry->d_name;

		if (strcmp(ext, ".param") == 0)
		{
			char		bin_filename[256];
			struct stat bin_st;

			/* Extract model name from param filename */
			model_start = strstr(base, "pgstar_model_");
			if (model_start)
			{
				model_start += strlen("pgstar_model_");
				strncpy(model_name, model_start, ext - model_start);
				model_name[ext - model_start] = '\0';
			}
			else
			{
				/* Default model name */
				strncpy(model_name, "default", sizeof(model_name) - 1);
			}

			/* Check if corresponding .bin file exists */
			snprintf(bin_filename, sizeof(bin_filename), "/opt/pgstar/models/pgstar_model_%s.bin", model_name);
			if (stat(bin_filename, &bin_st) == 0 && S_ISREG(bin_st.st_mode))
			{
				/* Both param and bin exist - add NCNN model */
				if (!first_entry)
					appendStringInfoString(buf, ",\n");
				appendStringInfoString(buf, "    {\n");
				appendStringInfo(buf, "      \"name\": \"%s\",\n", model_name);
				appendStringInfoString(buf, "      \"description\": \"Local NCNN model (ready to use)\",\n");
				appendStringInfoString(buf, "      \"downloads\": 0,\n");
				appendStringInfoString(buf, "      \"source\": \"Local\",\n");
				appendStringInfoString(buf, "      \"format\": \"NCNN\",\n");
				appendStringInfo(buf, "      \"url\": \"/opt/pgstar/models/pgstar_model_%s.param\",\n", model_name);
				appendStringInfo(buf, "      \"param_file\": \"pgstar_model_%s.param\",\n", model_name);
				appendStringInfo(buf, "      \"bin_file\": \"pgstar_model_%s.bin\"\n", model_name);
				appendStringInfoString(buf, "    }");
				first_entry = 0;
			}
		}
	}

	closedir(dir);

	appendStringInfoString(buf, "\n  ]\n");
	appendStringInfoString(buf, "}\n");
}

