#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>

/* Forward declarations */
static bool pgstar_convert_onnx_to_ncnn(const char *model_id, const char *model_name,
										char **param_file, char **bin_file, char **error_msg);
static bool pgstar_download_hf_file(const char *repo_id, const char *filename, const char *output_path);
static void pgstar_convert_build_response(const char *message, const char *param_file,
										 const char *bin_file, StringInfo buf);
static void pgstar_convert_build_error(const char *error, StringInfo buf);

void
pgstar_route_v1_models_convert(const PgstarRequest *req, PgstarResponse *resp)
{
	StringInfoData json_resp;
	char		model_id[256] = {0};
	char		model_name[256] = {0};
	char	   *param_file = NULL;
	char	   *bin_file = NULL;
	char	   *error_msg = NULL;

	elog(pgstar_log_level, "pgstar: v1/models/convert request: %s", req->method);

	if (pg_strcasecmp(req->method, "POST") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	if (!req->body || req->body_len == 0)
	{
		resp->status_code = 400;
		resp->status_text = "Bad Request";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"missing request body\"}\n");
		return;
	}

	/* Simple JSON parsing - extract model_id and model_name */
	{
		const char *p = req->body;
		const char *model_id_start = strstr(p, "\"model_id\"");
		const char *model_name_start = strstr(p, "\"model_name\"");
		
		if (model_id_start)
		{
			const char *quote = strchr(model_id_start + 10, '"');
			if (quote)
			{
				const char *value_start = quote + 1;
				const char *value_end = strchr(value_start, '"');
				if (value_end)
				{
					size_t len = Min(value_end - value_start, sizeof(model_id) - 1);
					strncpy(model_id, value_start, len);
					model_id[len] = '\0';
				}
			}
		}
		
		if (model_name_start)
		{
			const char *quote = strchr(model_name_start + 12, '"');
			if (quote)
			{
				const char *value_start = quote + 1;
				const char *value_end = strchr(value_start, '"');
				if (value_end)
				{
					size_t len = Min(value_end - value_start, sizeof(model_name) - 1);
					strncpy(model_name, value_start, len);
					model_name[len] = '\0';
				}
			}
		}
	}

	if (model_id[0] == '\0')
	{
		resp->status_code = 400;
		resp->status_text = "Bad Request";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"missing model_id\"}\n");
		return;
	}

	/* Perform conversion */
	if (pgstar_convert_onnx_to_ncnn(model_id, model_name[0] ? model_name : NULL,
									&param_file, &bin_file, &error_msg))
	{
		initStringInfo(&json_resp);
		pgstar_convert_build_response("Model converted successfully", param_file, bin_file, &json_resp);
		resp->status_code = 200;
		resp->status_text = "OK";
		resp->content_type = "application/json";
		resp->body = json_resp.data;

		if (param_file)
			pfree(param_file);
		if (bin_file)
			pfree(bin_file);
	}
	else
	{
		initStringInfo(&json_resp);
		pgstar_convert_build_error(error_msg ? error_msg : "Conversion failed", &json_resp);
		resp->status_code = 500;
		resp->status_text = "Internal Server Error";
		resp->content_type = "application/json";
		resp->body = json_resp.data;

		if (error_msg)
			pfree(error_msg);
	}
}

static bool
pgstar_convert_onnx_to_ncnn(const char *model_id, const char *model_name,
						   char **param_file, char **bin_file, char **error_msg)
{
	char	   *work_dir = NULL;
	char	   *onnx_file = NULL;
	char	   *param_path = NULL;
	char	   *bin_path = NULL;
	char	   *pnnx_cmd = NULL;
	pid_t		pid;
	int			status;
	struct stat st;

	/* Create work directory */
	{
		char	   *tmp_dir = psprintf("/opt/pgstar/tmp/convert_%d", (int) getpid());
		char	   *mkdir_cmd = psprintf("mkdir -p /opt/pgstar/tmp");
		int			mkdir_status;
		
		mkdir_status = system(mkdir_cmd);
		(void) mkdir_status;  /* Suppress unused result warning */
		pfree(mkdir_cmd);
		
		work_dir = tmp_dir;
		if (mkdir(work_dir, 0755) != 0)
		{
			*error_msg = psprintf("Failed to create work directory: %s", strerror(errno));
			pfree(work_dir);
			return false;
		}
	}

	/* Download ONNX model file - save to models directory */
	{
		char	   *onnx_filename = NULL;
		const char *model_safe_name = NULL;
		char	   *sanitized_name = NULL;
		const char *p;
		int			i;
		
		/* Extract model name from model_id (e.g., "onnx-community/LFM2-1.2B-ONNX" -> "LFM2-1.2B-ONNX") */
		p = strrchr(model_id, '/');
		if (p)
			model_safe_name = p + 1;
		else
			model_safe_name = model_id;
		
		/* Sanitize model name for filename */
		sanitized_name = palloc(strlen(model_safe_name) + 1);
		p = model_safe_name;
		i = 0;
		while (*p && i < 255)
		{
			if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || 
				(*p >= '0' && *p <= '9') || *p == '-' || *p == '_')
			{
				sanitized_name[i++] = *p;
			}
			else
			{
				sanitized_name[i++] = '_';
			}
			p++;
		}
		sanitized_name[i] = '\0';
		
		onnx_filename = psprintf("/opt/pgstar/models/pgstar_model_%s.onnx", sanitized_name);
		onnx_file = psprintf("%s/model.onnx", work_dir);
		
		/* Download to work dir first, then copy to models dir */
		if (!pgstar_download_hf_file(model_id, "onnx/model.onnx", onnx_file))
		{
			/* Try alternative paths */
			if (!pgstar_download_hf_file(model_id, "model.onnx", onnx_file))
			{
				*error_msg = psprintf("Failed to download ONNX model from Hugging Face: %s", model_id);
				pfree(work_dir);
				pfree(onnx_file);
				pfree(onnx_filename);
				pfree(sanitized_name);
				return false;
			}
		}
		
		/* Copy ONNX file to models directory for permanent storage */
		{
			char	   *copy_onnx_cmd;
			int			copy_onnx_status;
			
			copy_onnx_cmd = psprintf("cp %s %s", onnx_file, onnx_filename);
			copy_onnx_status = system(copy_onnx_cmd);
			if (copy_onnx_status == 0)
			{
				elog(pgstar_log_level, "pgstar: saved ONNX model to %s", onnx_filename);
			}
			pfree(copy_onnx_cmd);
		}
		
		pfree(onnx_filename);
		pfree(sanitized_name);
	}

	/* Check if pnnx is available */
	pnnx_cmd = NULL;
	
	/* Try common pnnx locations */
	if (access("/usr/local/bin/pnnx", X_OK) == 0)
	{
		pnnx_cmd = "/usr/local/bin/pnnx";
	}
	else if (access("pnnx", X_OK) == 0)
	{
		/* Check if pnnx is in PATH */
		pnnx_cmd = "pnnx";
	}
	else
	{
		*error_msg = psprintf("pnnx not found. Please install pnnx at /usr/local/bin/pnnx or ensure it's in PATH");
		pfree(work_dir);
		pfree(onnx_file);
		return false;
	}

	/* Run pnnx conversion */
	{
		char	   *cmd_str = psprintf("%s %s", pnnx_cmd, onnx_file);
		pid = fork();
		if (pid == 0)
		{
			/* Child process */
			execl("/bin/sh", "sh", "-c", cmd_str, (char *) NULL);
			pfree(cmd_str);
			_exit(1);
		}
		pfree(cmd_str);
	}
	
	if (pid < 0)
	{
		*error_msg = psprintf("Failed to fork process: %s", strerror(errno));
		pfree(work_dir);
		pfree(onnx_file);
		return false;
	}
	else
	{
		/* Parent process - wait for conversion */
		if (waitpid(pid, &status, 0) == -1)
		{
			*error_msg = psprintf("Failed to wait for conversion: %s", strerror(errno));
			pfree(work_dir);
			pfree(onnx_file);
			return false;
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			*error_msg = psprintf("pnnx conversion failed with exit code %d", WEXITSTATUS(status));
			pfree(work_dir);
			pfree(onnx_file);
			return false;
		}
	}

	/* Check for output files */
	param_path = psprintf("%s/model.ncnn.param", work_dir);
	bin_path = psprintf("%s/model.ncnn.bin", work_dir);

	if (stat(param_path, &st) != 0 || stat(bin_path, &st) != 0)
	{
		*error_msg = "Conversion completed but output files not found";
		pfree(work_dir);
		pfree(onnx_file);
		pfree(param_path);
		pfree(bin_path);
		return false;
	}

	/* Ensure model directory exists */
	{
		char	   *mkdir_cmd;
		int			mkdir_status;
		
		mkdir_cmd = psprintf("mkdir -p /opt/pgstar/models");
		mkdir_status = system(mkdir_cmd);
		(void) mkdir_status;  /* Suppress unused result warning */
		pfree(mkdir_cmd);
	}

	/* Extract model name from model_id for final filenames */
	{
		const char *p = strrchr(model_id, '/');
		const char *model_base = p ? p + 1 : model_id;
		char	   *sanitized_final = NULL;
		const char *q;
		int			j;
		
		sanitized_final = palloc(strlen(model_base) + 1);
		q = model_base;
		j = 0;
		while (*q && j < 255)
		{
			if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || 
				(*q >= '0' && *q <= '9') || *q == '-' || *q == '_')
			{
				sanitized_final[j++] = *q;
			}
			else
			{
				sanitized_final[j++] = '_';
			}
			q++;
		}
		sanitized_final[j] = '\0';
		
		*param_file = psprintf("/opt/pgstar/models/pgstar_model_%s.param", sanitized_final);
		*bin_file = psprintf("/opt/pgstar/models/pgstar_model_%s.bin", sanitized_final);
		
		pfree(sanitized_final);
	}

	/* Simple copy using system command (could be improved) */
	{
		char	   *copy_cmd;
		int			copy_status;
		
		copy_cmd = psprintf("cp %s %s && cp %s %s", param_path, *param_file, bin_path, *bin_file);
		copy_status = system(copy_cmd);
		if (copy_status != 0)
		{
			*error_msg = "Failed to copy converted files";
			pfree(work_dir);
			pfree(onnx_file);
			pfree(param_path);
			pfree(bin_path);
			pfree(copy_cmd);
			return false;
		}
		pfree(copy_cmd);
	}

	pfree(work_dir);
	pfree(onnx_file);
	pfree(param_path);
	pfree(bin_path);

	return true;
}

static bool
pgstar_download_hf_file(const char *repo_id, const char *filename, const char *output_path)
{
	char	   *url = NULL;
	char	   *cmd = NULL;
	int			status;

	/* Construct Hugging Face download URL */
	url = psprintf("https://huggingface.co/%s/resolve/main/%s", repo_id, filename);

	/* Use curl to download (since we have custom HTTP client limitations) */
	cmd = psprintf("curl -L -f -o %s '%s' 2>/dev/null", output_path, url);
	status = system(cmd);

	pfree(url);
	pfree(cmd);

	return (status == 0);
}

static void
pgstar_convert_build_response(const char *message, const char *param_file,
							 const char *bin_file, StringInfo buf)
{
	appendStringInfoString(buf, "{\n");
	appendStringInfoString(buf, "  \"success\": true,\n");
	appendStringInfo(buf, "  \"message\": \"%s\",\n", message);
	appendStringInfo(buf, "  \"param_file\": \"%s\",\n", param_file ? param_file : "");
	appendStringInfo(buf, "  \"bin_file\": \"%s\"\n", bin_file ? bin_file : "");
	appendStringInfoString(buf, "}\n");
}

static void
pgstar_convert_build_error(const char *error, StringInfo buf)
{
	appendStringInfoString(buf, "{\n");
	appendStringInfoString(buf, "  \"success\": false,\n");
	appendStringInfo(buf, "  \"error\": \"%s\"\n", error ? error : "Unknown error");
	appendStringInfoString(buf, "}\n");
}

