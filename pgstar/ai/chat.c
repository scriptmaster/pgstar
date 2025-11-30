#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* Forward declaration for c_ncnn library */
#ifdef HAVE_C_NCNN
#include "c_ncnn.h"
#endif

static void pgstar_v1_chat_parse_request(const char *body, size_t body_len,
										 char **messages, int *num_messages);
static void pgstar_v1_chat_build_response(const char *content, StringInfo buf);

#ifdef HAVE_C_NCNN
/* Static model state - initialized once */
static bool model_initialized = false;
static c_ncnn_model_t model_handle = NULL;
#endif

void
pgstar_route_v1_chat(const PgstarRequest *req, PgstarResponse *resp)
{
	StringInfoData json_resp;
	char	   *model_output = NULL;

	elog(pgstar_log_level, "pgstar: v1/chat/completions request: %s", req->method);

	if (pg_strcasecmp(req->method, "POST") != 0)
	{
		resp->status_code = 405;
		resp->status_text = "Method Not Allowed";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"method not allowed\"}\n");
		return;
	}

	/* Extract request body */
	if (!req->body || req->body_len == 0)
	{
		resp->status_code = 400;
		resp->status_text = "Bad Request";
		resp->content_type = "application/json";
		resp->body = pstrdup("{\"error\":\"missing request body\"}\n");
		return;
	}
	
	elog(pgstar_log_level, "pgstar: v1/chat/completions body_len=%zu", req->body_len);

#ifdef HAVE_C_NCNN
	{
	/* Initialize NCNN model if not already loaded */
	const char *param_path;
	const char *bin_path;
	const char *error_msg;
	c_ncnn_tensor_t *input;
	c_ncnn_tensor_t output_tensor;
	c_ncnn_error_t err;
	
	if (!model_initialized)
	{
		param_path = "/opt/pgstar/models/pgstar_model.param";
		bin_path = "/opt/pgstar/models/pgstar_model.bin";
		
		elog(pgstar_log_level, "pgstar: initializing NCNN model from %s", param_path);
		
		model_handle = c_ncnn_load_model(param_path, bin_path);
		if (model_handle == NULL)
		{
			/* Try ONNX model as fallback */
			const char *onnx_path = "/opt/pgstar/models/pgstar_model.onnx";
			struct stat st;
			
			if (stat(onnx_path, &st) == 0)
			{
				elog(pgstar_log_level, "pgstar: NCNN model not found, trying ONNX model from %s", onnx_path);
				model_handle = c_ncnn_load_onnx_model(onnx_path);
				if (model_handle != NULL)
				{
					elog(pgstar_log_level, "pgstar: ONNX model loaded successfully");
				}
			}
			
			if (model_handle == NULL)
			{
				error_msg = c_ncnn_get_error();
				elog(WARNING, "pgstar: failed to load model: %s", error_msg ? error_msg : "unknown error");
				/* Continue with placeholder response */
				model_output = pstrdup("Hello! Model not loaded, but endpoint is working. Please provide a model at /opt/pgstar/models/pgstar_model.param and /opt/pgstar/models/pgstar_model.bin, or /opt/pgstar/models/pgstar_model.onnx");
			}
		}
		else
		{
			model_initialized = true;
			elog(pgstar_log_level, "pgstar: NCNN model loaded successfully");
			
			/* Run inference */
			/* Create a simple input tensor (1x1x1 for text models, adjust as needed) */
			input = c_ncnn_alloc_tensor(1, 1, 1);
			if (input)
			{
				input->data[0] = 1.0f; /* Dummy input */
				
				err = c_ncnn_inference(model_handle, input, &output_tensor);
				if (err == C_NCNN_SUCCESS)
				{
					model_output = psprintf("Hello! Inference completed successfully. Output tensor: %dx%dx%d", 
										   output_tensor.width, output_tensor.height, output_tensor.channels);
					c_ncnn_free_tensor(&output_tensor);
				}
				else
				{
					error_msg = c_ncnn_get_error();
					model_output = psprintf("Inference failed: %s", error_msg ? error_msg : "unknown error");
				}
				c_ncnn_free_tensor(input);
			}
			else
			{
				model_output = pstrdup("Hello! Model loaded but failed to allocate input tensor.");
			}
		}
	}
	else
	{
		/* Model already loaded, run inference */
		input = c_ncnn_alloc_tensor(1, 1, 1);
		if (input)
		{
			input->data[0] = 1.0f;
			err = c_ncnn_inference(model_handle, input, &output_tensor);
			if (err == C_NCNN_SUCCESS)
			{
				model_output = psprintf("Hello! Inference completed. Output: %dx%dx%d", 
									   output_tensor.width, output_tensor.height, output_tensor.channels);
				c_ncnn_free_tensor(&output_tensor);
			}
			else
			{
				model_output = pstrdup("Hello! Inference failed.");
			}
			c_ncnn_free_tensor(input);
		}
		else
		{
			model_output = pstrdup("Hello! Failed to allocate input tensor.");
		}
	}
	}
#else
	/* Fallback when c_ncnn is not available */
	model_output = pstrdup("Hello! This is a placeholder response. NCNN library not available.");
#endif

	/* Build OpenAI-compatible response */
	initStringInfo(&json_resp);
	if (model_output == NULL)
		model_output = pstrdup("Hello! No response generated.");
	pgstar_v1_chat_build_response(model_output, &json_resp);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = "application/json";
	/* Use StringInfo data directly - it will be freed by the response handler */
	resp->body = json_resp.data;

	if (model_output)
		pgstar_safe_pfree(model_output);
}

static void
pgstar_v1_chat_build_response(const char *content, StringInfo buf)
{
	const char *p;
	
	/* StringInfo should already be initialized by caller */
	appendStringInfoString(buf, "{\n");
	appendStringInfoString(buf, "  \"id\": \"chatcmpl-pgstar-");
	appendStringInfo(buf, "%ld", (long) time(NULL));
	appendStringInfoString(buf, "\",\n");
	appendStringInfoString(buf, "  \"object\": \"chat.completion\",\n");
	appendStringInfoString(buf, "  \"created\": ");
	appendStringInfo(buf, "%ld", (long) time(NULL));
	appendStringInfoString(buf, ",\n");
	appendStringInfoString(buf, "  \"model\": \"pgstar-ncnn\",\n");
	appendStringInfoString(buf, "  \"choices\": [\n");
	appendStringInfoString(buf, "    {\n");
	appendStringInfoString(buf, "      \"index\": 0,\n");
	appendStringInfoString(buf, "      \"message\": {\n");
	appendStringInfoString(buf, "        \"role\": \"assistant\",\n");
	appendStringInfoString(buf, "        \"content\": \"");
	
	/* Escape JSON special characters in content */
	p = content;
	while (*p)
	{
		if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t')
		{
			appendStringInfoChar(buf, '\\');
			if (*p == '\n')
				appendStringInfoChar(buf, 'n');
			else if (*p == '\r')
				appendStringInfoChar(buf, 'r');
			else if (*p == '\t')
				appendStringInfoChar(buf, 't');
			else
				appendStringInfoChar(buf, *p);
		}
		else
		{
			appendStringInfoChar(buf, *p);
		}
		p++;
	}
	
	appendStringInfoString(buf, "\"\n");
	appendStringInfoString(buf, "      },\n");
	appendStringInfoString(buf, "      \"finish_reason\": \"stop\"\n");
	appendStringInfoString(buf, "    }\n");
	appendStringInfoString(buf, "  ],\n");
	appendStringInfoString(buf, "  \"usage\": {\n");
	appendStringInfoString(buf, "    \"prompt_tokens\": 0,\n");
	appendStringInfoString(buf, "    \"completion_tokens\": 0,\n");
	appendStringInfoString(buf, "    \"total_tokens\": 0\n");
	appendStringInfoString(buf, "  }\n");
	appendStringInfoString(buf, "}\n");
}

__attribute__((unused)) static void
pgstar_v1_chat_parse_request(const char *body, size_t body_len,
							char **messages, int *num_messages)
{
	/* TODO: Parse JSON request body to extract messages */
	/* For now, this is a placeholder */
	(void) body;  /* Suppress unused parameter warning */
	(void) body_len;  /* Suppress unused parameter warning */
	*messages = NULL;
	*num_messages = 0;
}

