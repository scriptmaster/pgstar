#include "postgres.h"

#include "pgstar.h"
#include "lib/stringinfo.h"

#include <string.h>

/* Forward declaration */
static void pgstar_v1_ui_models_build_html(const char *query, StringInfo buf);

void
pgstar_route_v1_ui_models(const PgstarRequest *req, PgstarResponse *resp)
{
	StringInfoData html_buf;
	char		query[256] = {0};
	const char *search_query = NULL;

	elog(pgstar_log_level, "pgstar: v1/ui/models request: %s", req->method);

	/* Extract search query parameter */
	if (req->query[0] != '\0')
	{
		if (pgstar_get_query_param(req->query, "q", query, sizeof(query)))
		{
			search_query = query;
		}
	}

	/* Build HTML response */
	initStringInfo(&html_buf);
	pgstar_v1_ui_models_build_html(search_query, &html_buf);

	resp->status_code = 200;
	resp->status_text = "OK";
	resp->content_type = "text/html; charset=utf-8";
	resp->body = html_buf.data;
}

static void
pgstar_v1_ui_models_build_html(const char *query, StringInfo buf)
{
	const char *search_value = query && query[0] != '\0' ? query : "";

	appendStringInfoString(buf, "<!DOCTYPE html>\n");
	appendStringInfoString(buf, "<html lang=\"en\">\n");
	appendStringInfoString(buf, "<head>\n");
	appendStringInfoString(buf, "  <meta charset=\"UTF-8\">\n");
	appendStringInfoString(buf, "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
	appendStringInfoString(buf, "  <title>NCNN Models - pgstar</title>\n");
	appendStringInfoString(buf, "  <link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/@picocss/pico@2/css/pico.min.css\">\n");
	appendStringInfoString(buf, "  <style>\n");
	appendStringInfoString(buf, "    .model-card { margin-bottom: 1rem; padding: 1rem; border: 1px solid var(--pico-muted-border-color); border-radius: var(--pico-border-radius); }\n");
	appendStringInfoString(buf, "    .model-card h3 { margin-top: 0; }\n");
	appendStringInfoString(buf, "    .model-meta { color: var(--pico-muted-color); font-size: 0.875rem; }\n");
	appendStringInfoString(buf, "    .download-btn { margin-top: 0.5rem; }\n");
	appendStringInfoString(buf, "    .source-badge { display: inline-block; padding: 0.25rem 0.5rem; background: var(--pico-primary-background); color: var(--pico-primary-inverse); border-radius: 0.25rem; font-size: 0.75rem; margin-left: 0.5rem; }\n");
	appendStringInfoString(buf, "  </style>\n");
	appendStringInfoString(buf, "</head>\n");
	appendStringInfoString(buf, "<body>\n");
	appendStringInfoString(buf, "  <main class=\"container\">\n");
	appendStringInfoString(buf, "    <header>\n");
	appendStringInfoString(buf, "      <h1>NCNN Models</h1>\n");
	appendStringInfoString(buf, "      <p>Search and download NCNN models from Hugging Face and other sources</p>\n");
	appendStringInfoString(buf, "    </header>\n");
	appendStringInfoString(buf, "    <form method=\"GET\" action=\"/v1/ui/models\">\n");
	appendStringInfoString(buf, "      <div class=\"grid\">\n");
	appendStringInfoString(buf, "        <div>\n");
	appendStringInfo(buf, "          <input type=\"text\" name=\"q\" placeholder=\"Search models...\" value=\"%s\" required>\n", search_value);
	appendStringInfoString(buf, "        </div>\n");
	appendStringInfoString(buf, "        <div>\n");
	appendStringInfoString(buf, "          <button type=\"submit\">Search</button>\n");
	appendStringInfoString(buf, "        </div>\n");
	appendStringInfoString(buf, "      </div>\n");
	appendStringInfoString(buf, "    </form>\n");

	/* Always show local models section */
	appendStringInfoString(buf, "    <section>\n");
	appendStringInfoString(buf, "      <h2>Local Models</h2>\n");
	appendStringInfoString(buf, "      <div id=\"local-models-list\">\n");
	appendStringInfoString(buf, "        <article class=\"model-card\">\n");
	appendStringInfoString(buf, "          <h3>Loading local models...</h3>\n");
	appendStringInfoString(buf, "          <p>Checking /opt/pgstar/models/...</p>\n");
	appendStringInfoString(buf, "        </article>\n");
	appendStringInfoString(buf, "      </div>\n");
	appendStringInfoString(buf, "    </section>\n");

	if (query && query[0] != '\0')
	{
		appendStringInfoString(buf, "    <section>\n");
		appendStringInfo(buf, "      <h2>Search Results for: %s</h2>\n", query);
		appendStringInfoString(buf, "      <div id=\"models-list\">\n");
		appendStringInfoString(buf, "        <article class=\"model-card\">\n");
		appendStringInfoString(buf, "          <h3>Loading models...</h3>\n");
		appendStringInfoString(buf, "          <p>Searching Hugging Face and other sources...</p>\n");
		appendStringInfoString(buf, "        </article>\n");
		appendStringInfoString(buf, "      </div>\n");
		appendStringInfoString(buf, "    </section>\n");

		/* JavaScript to fetch models from Hugging Face API */
		appendStringInfoString(buf, "    <script>\n");
		appendStringInfoString(buf, "      async function loadLocalModels() {\n");
		appendStringInfoString(buf, "        const list = document.getElementById('local-models-list');\n");
		appendStringInfoString(buf, "        try {\n");
		appendStringInfoString(buf, "          const response = await fetch('/v1/models/local');\n");
		appendStringInfoString(buf, "          if (response.ok) {\n");
		appendStringInfoString(buf, "            const localData = await response.json();\n");
		appendStringInfoString(buf, "            const models = localData.models || [];\n");
		appendStringInfoString(buf, "            if (models.length === 0) {\n");
		appendStringInfoString(buf, "              list.innerHTML = '<article class=\"model-card\"><h3>No local models</h3><p>Download and convert models to see them here.</p></article>';\n");
		appendStringInfoString(buf, "              return [];\n");
		appendStringInfoString(buf, "            }\n");
		appendStringInfoString(buf, "            list.innerHTML = models.map(model => {\n");
		appendStringInfoString(buf, "              const downloads = model.downloads ? ` • ${model.downloads.toLocaleString()} downloads` : '';\n");
		appendStringInfoString(buf, "              const filesInfo = model.files && model.files.length > 0 ? `<p class=\"model-meta\">Files: ${model.files.slice(0, 3).join(', ')}${model.files.length > 3 ? '...' : ''}</p>` : '';\n");
		appendStringInfoString(buf, "              const formatBadge = model.format === 'ONNX' ? '<span class=\"source-badge\" style=\"background: #ff9800;\">ONNX</span>' : '<span class=\"source-badge\" style=\"background: #4caf50;\">NCNN</span>';\n");
		appendStringInfoString(buf, "              const convertBtn = model.format === 'ONNX' ? `<button class=\"download-btn\" onclick=\"convertModel('${model.name}', '${model.url}')\" style=\"background: var(--pico-primary-background);\">Convert to NCNN</button>` : '';\n");
		appendStringInfoString(buf, "              return `\n");
		appendStringInfoString(buf, "                <article class=\"model-card\">\n");
		appendStringInfoString(buf, "                  <h3>${model.name} <span class=\"source-badge\">${model.source}</span>${formatBadge}</h3>\n");
		appendStringInfoString(buf, "                  <p>${model.description}${downloads}</p>\n");
		appendStringInfoString(buf, "                  ${filesInfo}\n");
		appendStringInfoString(buf, "                  <div class=\"grid\">\n");
		appendStringInfoString(buf, "                    <div><a href=\"${model.url}\" target=\"_blank\" role=\"button\" class=\"secondary\">View Details</a></div>\n");
		appendStringInfoString(buf, "                    <div>${convertBtn || `<button class=\"download-btn\" onclick=\"alert('Model ready to use!')\">Ready</button>`}</div>\n");
		appendStringInfoString(buf, "                  </div>\n");
		appendStringInfoString(buf, "                </article>\n");
		appendStringInfoString(buf, "              `;\n");
		appendStringInfoString(buf, "            }).join('');\n");
		appendStringInfoString(buf, "            return models;\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        } catch (e) {\n");
		appendStringInfoString(buf, "          console.error('Failed to load local models:', e);\n");
		appendStringInfoString(buf, "          list.innerHTML = '<article class=\"model-card\"><h3>Error loading local models</h3><p>Check server logs for details.</p></article>';\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "        return [];\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "      \n");
		appendStringInfoString(buf, "      async function searchModels(query) {\n");
		appendStringInfoString(buf, "        const list = document.getElementById('models-list');\n");
		appendStringInfoString(buf, "        list.innerHTML = '<article class=\"model-card\"><h3>Searching...</h3><p>Please wait...</p></article>';\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        const models = [];\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        // Search Hugging Face for NCNN models\n");
		appendStringInfoString(buf, "        try {\n");
		appendStringInfoString(buf, "          const hfResponse = await fetch(`https://huggingface.co/api/models?search=${encodeURIComponent(query)}&filter=ncnn&sort=downloads&direction=-1&limit=20`);\n");
		appendStringInfoString(buf, "          if (hfResponse.ok) {\n");
		appendStringInfoString(buf, "            const hfData = await hfResponse.json();\n");
		appendStringInfoString(buf, "            hfData.forEach(model => {\n");
		appendStringInfoString(buf, "              models.push({\n");
		appendStringInfoString(buf, "                name: model.id || model.modelId,\n");
		appendStringInfoString(buf, "                description: model.pipeline_tag || 'NCNN Model',\n");
		appendStringInfoString(buf, "                downloads: model.downloads || 0,\n");
		appendStringInfoString(buf, "                source: 'Hugging Face',\n");
		appendStringInfoString(buf, "                format: 'NCNN',\n");
		appendStringInfoString(buf, "                url: `https://huggingface.co/${model.id || model.modelId}`,\n");
		appendStringInfoString(buf, "                files: model.siblings ? model.siblings.filter(f => f.rfilename.endsWith('.param') || f.rfilename.endsWith('.bin')).map(f => f.rfilename) : []\n");
		appendStringInfoString(buf, "              });\n");
		appendStringInfoString(buf, "            });\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        } catch (e) {\n");
		appendStringInfoString(buf, "          console.error('Hugging Face NCNN search failed:', e);\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        // If no NCNN models found, search for ONNX models\n");
		appendStringInfoString(buf, "        if (models.length === 0) {\n");
		appendStringInfoString(buf, "          try {\n");
		appendStringInfoString(buf, "            const onnxResponse = await fetch(`https://huggingface.co/api/models?search=${encodeURIComponent(query)}&filter=onnx&sort=downloads&direction=-1&limit=20`);\n");
		appendStringInfoString(buf, "            if (onnxResponse.ok) {\n");
		appendStringInfoString(buf, "              const onnxData = await onnxResponse.json();\n");
		appendStringInfoString(buf, "              onnxData.forEach(model => {\n");
		appendStringInfoString(buf, "                // If filter=onnx returned this model, it has ONNX files\n");
		appendStringInfoString(buf, "                // Note: siblings field not included in basic API response\n");
		appendStringInfoString(buf, "                models.push({\n");
		appendStringInfoString(buf, "                  name: model.id || model.modelId,\n");
		appendStringInfoString(buf, "                  description: (model.pipeline_tag || 'ONNX Model') + ' (can convert to NCNN)',\n");
		appendStringInfoString(buf, "                  downloads: model.downloads || 0,\n");
		appendStringInfoString(buf, "                  source: 'Hugging Face',\n");
		appendStringInfoString(buf, "                  format: 'ONNX',\n");
		appendStringInfoString(buf, "                  url: `https://huggingface.co/${model.id || model.modelId}`,\n");
		appendStringInfoString(buf, "                  files: []  // Siblings not available in basic API response\n");
		appendStringInfoString(buf, "                });\n");
		appendStringInfoString(buf, "              });\n");
		appendStringInfoString(buf, "            }\n");
		appendStringInfoString(buf, "          } catch (e) {\n");
		appendStringInfoString(buf, "            console.error('Hugging Face ONNX search failed:', e);\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        // Add known GitHub sources\n");
		appendStringInfoString(buf, "        const githubSources = [\n");
		appendStringInfoString(buf, "          { name: 'Baiyuetribe/ncnn-models', url: 'https://github.com/Baiyuetribe/ncnn-models', description: 'Collection of AI models compatible with NCNN' },\n");
		appendStringInfoString(buf, "          { name: 'TNTwise/Universal-NCNN-Upscaler', url: 'https://github.com/TNTwise/Universal-NCNN-Upscaler', description: 'ESRGAN and Real-ESRGAN models' },\n");
		appendStringInfoString(buf, "          { name: 'EdVince/Stable-Diffusion-NCNN', url: 'https://github.com/EdVince/Stable-Diffusion-NCNN', description: 'Stable Diffusion using NCNN' },\n");
		appendStringInfoString(buf, "          { name: 'nihui/waifu2x-ncnn-vulkan', url: 'https://github.com/nihui/waifu2x-ncnn-vulkan', description: 'waifu2x image upscaler' }\n");
		appendStringInfoString(buf, "        ];\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        githubSources.forEach(source => {\n");
		appendStringInfoString(buf, "          if (source.name.toLowerCase().includes(query.toLowerCase()) || source.description.toLowerCase().includes(query.toLowerCase())) {\n");
		appendStringInfoString(buf, "            models.push({\n");
		appendStringInfoString(buf, "              name: source.name,\n");
		appendStringInfoString(buf, "              description: source.description,\n");
		appendStringInfoString(buf, "              downloads: 0,\n");
		appendStringInfoString(buf, "              source: 'GitHub',\n");
		appendStringInfoString(buf, "              url: source.url,\n");
		appendStringInfoString(buf, "              files: []\n");
		appendStringInfoString(buf, "            });\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        });\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        // Render models\n");
		appendStringInfoString(buf, "        if (models.length === 0) {\n");
		appendStringInfoString(buf, "          list.innerHTML = '<article class=\"model-card\"><h3>No models found</h3><p>Try a different search term.</p></article>';\n");
		appendStringInfoString(buf, "          return;\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        // Sort: local models first, then by downloads\n");
		appendStringInfoString(buf, "        models.sort((a, b) => {\n");
		appendStringInfoString(buf, "          if (a.source === 'Local' && b.source !== 'Local') return -1;\n");
		appendStringInfoString(buf, "          if (a.source !== 'Local' && b.source === 'Local') return 1;\n");
		appendStringInfoString(buf, "          return (b.downloads || 0) - (a.downloads || 0);\n");
		appendStringInfoString(buf, "        });\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        list.innerHTML = models.map(model => {\n");
		appendStringInfoString(buf, "          const downloads = model.downloads ? ` • ${model.downloads.toLocaleString()} downloads` : '';\n");
		appendStringInfoString(buf, "          const filesInfo = model.files && model.files.length > 0 ? `<p class=\"model-meta\">Files: ${model.files.slice(0, 3).join(', ')}${model.files.length > 3 ? '...' : ''}</p>` : '';\n");
		appendStringInfoString(buf, "          const formatBadge = model.format === 'ONNX' ? '<span class=\"source-badge\" style=\"background: #ff9800;\">ONNX</span>' : '';\n");
		appendStringInfoString(buf, "          const convertBtn = model.format === 'ONNX' ? `<button class=\"download-btn\" onclick=\"convertModel('${model.name}', '${model.url}')\" style=\"background: var(--pico-primary-background);\">Convert to NCNN</button>` : '';\n");
		appendStringInfoString(buf, "          return `\n");
		appendStringInfoString(buf, "            <article class=\"model-card\">\n");
		appendStringInfoString(buf, "              <h3>${model.name} <span class=\"source-badge\">${model.source}</span>${formatBadge}</h3>\n");
		appendStringInfoString(buf, "              <p>${model.description}${downloads}</p>\n");
		appendStringInfoString(buf, "              ${filesInfo}\n");
		appendStringInfoString(buf, "              <div class=\"grid\">\n");
		appendStringInfoString(buf, "                <div><a href=\"${model.url}\" target=\"_blank\" role=\"button\" class=\"secondary\">View Details</a></div>\n");
		appendStringInfoString(buf, "                <div>${convertBtn || `<button class=\"download-btn\" onclick=\"downloadModel('${model.name}', '${model.url}')\">Download Info</button>`}</div>\n");
		appendStringInfoString(buf, "              </div>\n");
		appendStringInfoString(buf, "            </article>\n");
		appendStringInfoString(buf, "          `;\n");
		appendStringInfoString(buf, "        }).join('');\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "      \n");
		appendStringInfoString(buf, "      function downloadModel(name, url) {\n");
		appendStringInfoString(buf, "        alert(`To download ${name}, visit: ${url}\\n\\nFor Hugging Face models, look for files ending in .param and .bin in the Files tab.`);\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "      \n");
		appendStringInfoString(buf, "      async function convertModel(name, url) {\n");
		appendStringInfoString(buf, "        const modelId = url.replace('https://huggingface.co/', '');\n");
		appendStringInfoString(buf, "        const confirmMsg = `Convert ${name} from ONNX to NCNN?\\n\\nThis will:\\n1. Download the ONNX model\\n2. Convert it using pnnx\\n3. Save as NCNN format (.param + .bin)\\n\\nModel ID: ${modelId}`;\n");
		appendStringInfoString(buf, "        if (!confirm(confirmMsg)) return;\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        const statusDiv = document.createElement('div');\n");
		appendStringInfoString(buf, "        statusDiv.innerHTML = `<article class=\"model-card\"><h3>Converting ${name}...</h3><p>Please wait, this may take a few minutes.</p><progress></progress></article>`;\n");
		appendStringInfoString(buf, "        document.getElementById('models-list').prepend(statusDiv);\n");
		appendStringInfoString(buf, "        \n");
		appendStringInfoString(buf, "        try {\n");
		appendStringInfoString(buf, "          const response = await fetch('/v1/models/convert', {\n");
		appendStringInfoString(buf, "            method: 'POST',\n");
		appendStringInfoString(buf, "            headers: { 'Content-Type': 'application/json' },\n");
		appendStringInfoString(buf, "            body: JSON.stringify({ model_id: modelId, model_name: name })\n");
		appendStringInfoString(buf, "          });\n");
		appendStringInfoString(buf, "          \n");
		appendStringInfoString(buf, "          const result = await response.json();\n");
		appendStringInfoString(buf, "          \n");
		appendStringInfoString(buf, "          if (response.ok) {\n");
		appendStringInfoString(buf, "            statusDiv.innerHTML = `<article class=\"model-card\" style=\"background: #d4edda;\"><h3>✓ Conversion Complete</h3><p>${result.message || 'Model converted successfully'}</p><p><strong>Files:</strong> ${result.param_file || 'N/A'} and ${result.bin_file || 'N/A'}</p><p><small>You can now use this model with /v1/chat/completions</small></p></article>`;\n");
		appendStringInfoString(buf, "          } else {\n");
		appendStringInfoString(buf, "            statusDiv.innerHTML = `<article class=\"model-card\" style=\"background: #f8d7da;\"><h3>✗ Conversion Failed</h3><p>${result.error || 'Unknown error occurred'}</p></article>`;\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        } catch (e) {\n");
		appendStringInfoString(buf, "          statusDiv.innerHTML = `<article class=\"model-card\" style=\"background: #f8d7da;\"><h3>✗ Error</h3><p>${e.message}</p></article>`;\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "      \n");
		appendStringInfoString(buf, "      // Load local models on page load\n");
		appendStringInfoString(buf, "      loadLocalModels();\n");
		appendStringInfoString(buf, "      \n");
		appendStringInfo(buf, "      // Auto-search on page load if query exists\n");
		appendStringInfo(buf, "      if ('%s') {\n", search_value);
		appendStringInfoString(buf, "        searchModels(decodeURIComponent('");
		/* Escape the query for JavaScript */
		if (query && query[0] != '\0')
		{
			const char *p = query;
			while (*p)
			{
				if (*p == '\\' || *p == '\'' || *p == '"')
					appendStringInfoChar(buf, '\\');
				appendStringInfoChar(buf, *p);
				p++;
			}
		}
		appendStringInfoString(buf, "'));\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "    </script>\n");
	}
	else
	{
		/* JavaScript to load local models even when no search query */
		appendStringInfoString(buf, "    <script>\n");
		appendStringInfoString(buf, "      async function loadLocalModels() {\n");
		appendStringInfoString(buf, "        const list = document.getElementById('local-models-list');\n");
		appendStringInfoString(buf, "        try {\n");
		appendStringInfoString(buf, "          const response = await fetch('/v1/models/local');\n");
		appendStringInfoString(buf, "          if (response.ok) {\n");
		appendStringInfoString(buf, "            const localData = await response.json();\n");
		appendStringInfoString(buf, "            const models = localData.models || [];\n");
		appendStringInfoString(buf, "            if (models.length === 0) {\n");
		appendStringInfoString(buf, "              list.innerHTML = '<article class=\"model-card\"><h3>No local models</h3><p>Download and convert models to see them here.</p></article>';\n");
		appendStringInfoString(buf, "              return;\n");
		appendStringInfoString(buf, "            }\n");
		appendStringInfoString(buf, "            list.innerHTML = models.map(model => {\n");
		appendStringInfoString(buf, "              const downloads = model.downloads ? ` • ${model.downloads.toLocaleString()} downloads` : '';\n");
		appendStringInfoString(buf, "              const filesInfo = model.files && model.files.length > 0 ? `<p class=\"model-meta\">Files: ${model.files.slice(0, 3).join(', ')}${model.files.length > 3 ? '...' : ''}</p>` : '';\n");
		appendStringInfoString(buf, "              const formatBadge = model.format === 'ONNX' ? '<span class=\"source-badge\" style=\"background: #ff9800;\">ONNX</span>' : '<span class=\"source-badge\" style=\"background: #4caf50;\">NCNN</span>';\n");
		appendStringInfoString(buf, "              const convertBtn = model.format === 'ONNX' ? `<button class=\"download-btn\" onclick=\"convertModel('${model.name}', '${model.url}')\" style=\"background: var(--pico-primary-background);\">Convert to NCNN</button>` : '';\n");
		appendStringInfoString(buf, "              return `\n");
		appendStringInfoString(buf, "                <article class=\"model-card\">\n");
		appendStringInfoString(buf, "                  <h3>${model.name} <span class=\"source-badge\">${model.source}</span>${formatBadge}</h3>\n");
		appendStringInfoString(buf, "                  <p>${model.description}${downloads}</p>\n");
		appendStringInfoString(buf, "                  ${filesInfo}\n");
		appendStringInfoString(buf, "                  <div class=\"grid\">\n");
		appendStringInfoString(buf, "                    <div><a href=\"${model.url}\" target=\"_blank\" role=\"button\" class=\"secondary\">View Details</a></div>\n");
		appendStringInfoString(buf, "                    <div>${convertBtn || `<button class=\"download-btn\" onclick=\"alert('Model ready to use!')\">Ready</button>`}</div>\n");
		appendStringInfoString(buf, "                  </div>\n");
		appendStringInfoString(buf, "                </article>\n");
		appendStringInfoString(buf, "              `;\n");
		appendStringInfoString(buf, "            }).join('');\n");
		appendStringInfoString(buf, "          }\n");
		appendStringInfoString(buf, "        } catch (e) {\n");
		appendStringInfoString(buf, "          console.error('Failed to load local models:', e);\n");
		appendStringInfoString(buf, "          list.innerHTML = '<article class=\"model-card\"><h3>Error loading local models</h3><p>Check server logs for details.</p></article>';\n");
		appendStringInfoString(buf, "        }\n");
		appendStringInfoString(buf, "      }\n");
		appendStringInfoString(buf, "      loadLocalModels();\n");
		appendStringInfoString(buf, "    </script>\n");
		appendStringInfoString(buf, "    \n");
		appendStringInfoString(buf, "    <section>\n");
		appendStringInfoString(buf, "      <h2>Popular Sources</h2>\n");
		appendStringInfoString(buf, "      <div class=\"grid\">\n");
		appendStringInfoString(buf, "        <article class=\"model-card\">\n");
		appendStringInfoString(buf, "          <h3>Hugging Face <span class=\"source-badge\">Primary</span></h3>\n");
		appendStringInfoString(buf, "          <p>Search thousands of NCNN models on Hugging Face. Use the search box above to find models.</p>\n");
		appendStringInfoString(buf, "          <a href=\"https://huggingface.co/models?library=ncnn\" target=\"_blank\" role=\"button\">Browse Hugging Face</a>\n");
		appendStringInfoString(buf, "        </article>\n");
		appendStringInfoString(buf, "        <article class=\"model-card\">\n");
		appendStringInfoString(buf, "          <h3>GitHub Repositories <span class=\"source-badge\">GitHub</span></h3>\n");
		appendStringInfoString(buf, "          <p>Community-maintained NCNN model collections on GitHub.</p>\n");
		appendStringInfoString(buf, "          <ul>\n");
		appendStringInfoString(buf, "            <li><a href=\"https://github.com/Baiyuetribe/ncnn-models\" target=\"_blank\">Baiyuetribe/ncnn-models</a></li>\n");
		appendStringInfoString(buf, "            <li><a href=\"https://github.com/TNTwise/Universal-NCNN-Upscaler\" target=\"_blank\">TNTwise/Universal-NCNN-Upscaler</a></li>\n");
		appendStringInfoString(buf, "            <li><a href=\"https://github.com/EdVince/Stable-Diffusion-NCNN\" target=\"_blank\">EdVince/Stable-Diffusion-NCNN</a></li>\n");
		appendStringInfoString(buf, "            <li><a href=\"https://github.com/nihui/waifu2x-ncnn-vulkan\" target=\"_blank\">nihui/waifu2x-ncnn-vulkan</a></li>\n");
		appendStringInfoString(buf, "          </ul>\n");
		appendStringInfoString(buf, "        </article>\n");
		appendStringInfoString(buf, "      </div>\n");
		appendStringInfoString(buf, "    </section>\n");
	}

	appendStringInfoString(buf, "    <footer style=\"margin-top: 3rem; padding-top: 2rem; border-top: 1px solid var(--pico-muted-border-color);\">\n");
	appendStringInfoString(buf, "      <p><small>pgstar AI Models Browser • <a href=\"/pgstar/version\">pgstar version</a></small></p>\n");
	appendStringInfoString(buf, "    </footer>\n");
	appendStringInfoString(buf, "  </main>\n");
	appendStringInfoString(buf, "</body>\n");
	appendStringInfoString(buf, "</html>\n");
}

