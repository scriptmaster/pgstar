# c_ncnn - C Wrapper Library for NCNN and ONNX Runtime

A C-compatible wrapper library that provides a unified interface for loading and running inference with NCNN and ONNX models.

## Features

- **NCNN Support**: Load and run inference with NCNN models (.param/.bin files)
- **ONNX Support**: Load and run inference with ONNX models (.onnx files)
- **Model Downloading**: Download models from URLs with caching
- **C-Compatible API**: Can be used from C code (e.g., PostgreSQL extensions)

## Building

### Prerequisites

- C++11 compiler (g++ or clang++)
- Make

**Optional Dependencies** (library will build without them):
- NCNN library (for NCNN model support) - if not available, stub implementation is used
- ONNX Runtime (for ONNX model support) - if not available, stub implementation is used
- libcurl (for model downloading from URLs) - if not available, only local file loading works

### Build

```bash
cd c_ncnn
make
```

This will build:
- `build/libc_ncnn.so` - Shared library
- `build/libc_ncnn.a` - Static library

### Install

```bash
make install
```

This installs to `/usr/local` by default. Override with:
```bash
make install INSTALL_PREFIX=/custom/path
```

## Usage

### Basic Example

```c
#include "c_ncnn.h"

// Load a model
c_ncnn_model_t model = c_ncnn_load_model("model.param", "model.bin");
if (!model) {
    printf("Error: %s\n", c_ncnn_get_error());
    return 1;
}

// Prepare input tensor
c_ncnn_tensor_t* input = c_ncnn_alloc_tensor(224, 224, 3);
// ... fill input->data ...

// Run inference
c_ncnn_tensor_t output;
c_ncnn_error_t err = c_ncnn_inference(model, input, &output);
if (err == C_NCNN_SUCCESS) {
    // Use output->data
    printf("Output: %dx%dx%d\n", output.width, output.height, output.channels);
    c_ncnn_free_tensor(&output);
}

// Cleanup
c_ncnn_free_tensor(input);
c_ncnn_unload_model(model);
```

### Downloading Models

```c
// Download and load a model from URL
c_ncnn_model_t model = c_ncnn_download_and_load(
    "https://example.com/model.param",
    C_NCNN_FORMAT_NCNN,
    "/tmp/models"  // cache directory
);
```

## API Reference

See `include/c_ncnn.h` for complete API documentation.

## Integration with pgstar

The pgstar PostgreSQL extension uses this library to provide AI endpoints. The library is automatically built when building pgstar if it's located at `../c_ncnn`.

## Notes

- **Built-in HTTP client** - No external dependencies needed for downloading models
  - Uses custom HTTP-only client (no HTTPS support)
  - Supports downloading models from HTTP URLs via `c_ncnn_download_and_load()`
  - For HTTPS URLs, download manually and use `c_ncnn_load_model()` instead
- **Zero dependencies** - The library builds with no external libraries (except optional NCNN/ONNX)
- The library works as a stub if NCNN/ONNX libraries are not available
- All memory allocated by the library should be freed using the provided free functions

See `WHY_CUSTOM_HTTP.md` for details on why we use a custom HTTP client instead of libcurl.


