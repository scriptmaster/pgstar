# AI Endpoint Implementation

## Overview

This document describes the AI endpoint implementation for pgstar, including the `/v1/chat/completions` endpoint and the `c_ncnn` C wrapper library.

## Architecture

### Components

1. **Router (`pgstar/http/router.c`)**: Routes `/v1/` paths to the v1 handler
2. **V1 Router (`pgstar/http/v1.c`)**: Routes `/v1/chat/completions` to the chat handler
3. **Chat Handler (`pgstar/ai/chat.c`)**: Implements the chat completions endpoint
4. **c_ncnn Library (`c_ncnn/`)**: C wrapper library for NCNN and ONNX models

### Endpoint

- **Path**: `/v1/chat/completions`
- **Method**: POST
- **Request Body**: JSON with OpenAI-compatible format
- **Response**: JSON with OpenAI-compatible format

## Building

### Build c_ncnn Library

```bash
cd c_ncnn
make
```

This creates:
- `build/libc_ncnn.so` - Shared library
- `build/libc_ncnn.a` - Static library

### Build pgstar Extension

The pgstar extension will automatically build c_ncnn if it's located at `../c_ncnn`:

```bash
cd pgstar
make
```

Or from the root:

```bash
make extension-build-pgstar
```

## Usage

### 1. Prepare Model Files

Place your NCNN model files in `/opt/pgstar/models/`:
- `/opt/pgstar/models/pgstar_model.param` - Model parameter file
- `/opt/pgstar/models/pgstar_model.bin` - Model binary file

Or use the test script:

```bash
cd c_ncnn
./test_model.sh
mkdir -p /opt/pgstar/models
cp /tmp/pgstar_models/test_model.param /opt/pgstar/models/pgstar_model.param
cp /tmp/pgstar_models/test_model.bin /opt/pgstar/models/pgstar_model.bin
```

### 2. Start PostgreSQL with pgstar

Ensure pgstar is configured in `postgresql.conf`:

```
shared_preload_libraries = 'pgstar'
pgstar.port = 5481
pgstar.log_level = 'info'
```

### 3. Test the Endpoint

```bash
curl -X POST http://localhost:5481/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "pgstar-ncnn",
    "messages": [
      {"role": "user", "content": "Hello, how are you?"}
    ]
  }'
```

### Expected Response

```json
{
  "id": "chatcmpl-pgstar-1234567890",
  "object": "chat.completion",
  "created": 1234567890,
  "model": "pgstar-ncnn",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello! Inference completed successfully..."
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 0,
    "completion_tokens": 0,
    "total_tokens": 0
  }
}
```

## c_ncnn Library API

### Model Loading

```c
// Load NCNN model
c_ncnn_model_t model = c_ncnn_load_model("model.param", "model.bin");

// Load ONNX model
c_ncnn_model_t model = c_ncnn_load_onnx_model("model.onnx");

// Download and load
c_ncnn_model_t model = c_ncnn_download_and_load(
    "https://example.com/model.param",
    C_NCNN_FORMAT_NCNN,
    "/tmp/models"
);
```

### Inference

```c
// Allocate input tensor
c_ncnn_tensor_t* input = c_ncnn_alloc_tensor(224, 224, 3);
// ... fill input->data ...

// Run inference
c_ncnn_tensor_t output;
c_ncnn_error_t err = c_ncnn_inference(model, input, &output);

// Cleanup
c_ncnn_free_tensor(&output);
c_ncnn_free_tensor(input);
c_ncnn_unload_model(model);
```

## Implementation Status

### Completed

- ✅ Router setup for `/v1/` paths
- ✅ Chat completions endpoint handler
- ✅ c_ncnn C wrapper library structure
- ✅ NCNN model loading (stub implementation)
- ✅ ONNX model loading (stub implementation)
- ✅ Model downloading functionality (requires libcurl)
- ✅ Integration with pgstar build system
- ✅ OpenAI-compatible response format

### Pending/Enhancement

- 🔄 Full NCNN inference implementation (requires NCNN library)
- 🔄 Full ONNX inference implementation (requires ONNX Runtime)
- 🔄 JSON request parsing for messages
- 🔄 Token counting
- 🔄 Model conversion utilities (.pt to .ncnn, .pt to .onnx, .onnx to .ncnn)

## Notes

- The current implementation includes stub code that allows compilation and basic functionality without NCNN/ONNX libraries
- To enable full functionality, install NCNN and/or ONNX Runtime and rebuild
- Model downloading requires libcurl
- The endpoint currently returns placeholder responses until a real model is loaded and inference is implemented

## Troubleshooting

### Model Not Loading

Check that model files exist:
```bash
ls -lh /opt/pgstar/models/pgstar_model.*
```

Check PostgreSQL logs for errors:
```bash
make log
```

### Library Not Found

Ensure c_ncnn is built:
```bash
cd c_ncnn && make
```

Check that the library exists:
```bash
ls -lh c_ncnn/build/libc_ncnn.so
```

### Compilation Errors

If you see errors about missing NCNN/ONNX headers, the stub implementation should still work. The code is designed to compile without these dependencies.


