# ONNX Model Support

## Overview

pgstar now supports both NCNN and ONNX models for the `/v1/chat/completions` endpoint. When NCNN models are not found, the system automatically searches for ONNX models and provides conversion capabilities.

## Features

### 1. Dual Format Support
- **NCNN Models**: Primary format (`.param` + `.bin` files)
- **ONNX Models**: Secondary format (`.onnx` files) with automatic fallback

### 2. Model Search
The `/v1/ui/models` endpoint now:
- Searches for NCNN models first
- If no NCNN models found, searches for ONNX models
- Displays format badges (NCNN/ONNX) for each model
- Shows "Convert to NCNN" button for ONNX models

### 3. ONNX to NCNN Conversion
- **Endpoint**: `POST /v1/models/convert`
- **Tool**: Uses `pnnx` (Python package) for conversion
- **Process**:
  1. Downloads ONNX model from Hugging Face
  2. Converts using `pnnx` command-line tool
  3. Saves as NCNN format (`.param` + `.bin`)
  4. Places files in `/tmp/` for use with chat endpoint

## Usage

### Search for Models

```bash
# Search (will show NCNN first, then ONNX if empty)
curl "http://localhost:5480/v1/ui/models?q=LFM2"
```

### Convert ONNX Model

```bash
curl -X POST http://localhost:5480/v1/models/convert \
  -H 'Content-Type: application/json' \
  -d '{
    "model_id": "onnx-community/LFM2-1.2B-ONNX",
    "model_name": "LFM2-1.2B"
  }'
```

Response:
```json
{
  "success": true,
  "message": "Model converted successfully",
  "param_file": "/opt/pgstar/models/pgstar_model_LFM2-1.2B.param",
  "bin_file": "/opt/pgstar/models/pgstar_model_LFM2-1.2B.bin"
}
```

### Use Converted Model

After conversion, the model files are placed in `/tmp/`. The chat endpoint will automatically detect and use them:

```bash
curl -X POST http://localhost:5480/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "messages": [{"role": "user", "content": "Hello!"}]
  }'
```

## Model Loading Priority

The `/v1/chat/completions` endpoint loads models in this order:

1. **NCNN Model**: `/opt/pgstar/models/pgstar_model.param` + `/opt/pgstar/models/pgstar_model.bin`
2. **ONNX Model** (fallback): `/opt/pgstar/models/pgstar_model.onnx`
3. **Placeholder**: Returns helpful message if no model found

## Conversion Requirements

### Prerequisites
- `pnnx` must be installed (via `pip install pnnx` or `python3 -m pnnx`)
- `curl` must be available for downloading models
- Sufficient disk space in `/tmp/`

### Installation

```bash
# Install pnnx
pip install pnnx

# Or use Python module
python3 -m pip install pnnx
```

### Conversion Process

The conversion endpoint:
1. Creates a temporary work directory
2. Downloads the ONNX model file from Hugging Face
3. Runs `pnnx` to convert ONNX → NCNN
4. Copies output files to `/tmp/` with model name
5. Cleans up temporary files

## Example: LFM2-1.2B-ONNX

The [LFM2-1.2B-ONNX model](https://huggingface.co/onnx-community/LFM2-1.2B-ONNX) is a great example:

1. **Search**: `http://localhost:5480/v1/ui/models?q=LFM2`
2. **Click**: "Convert to NCNN" button
3. **Wait**: Conversion completes (may take a few minutes)
4. **Use**: Model is ready for `/v1/chat/completions`

## Model Sources

### Hugging Face
- **NCNN Models**: Filtered by `filter=ncnn`
- **ONNX Models**: Filtered by `filter=onnx`
- Both searched automatically based on availability

### File Locations
- **Downloaded ONNX**: `/tmp/pgstar_convert_<pid>/model.onnx` (temporary)
- **Converted NCNN**: `/opt/pgstar/models/pgstar_model_<name>.param` and `.bin`
- **Final Model**: `/opt/pgstar/models/pgstar_model.param` and `.bin` (for chat endpoint)

## Error Handling

### Common Issues

1. **pnnx not found**
   - Error: "pnnx conversion failed"
   - Solution: Install pnnx via pip

2. **Download failed**
   - Error: "Failed to download ONNX model"
   - Solution: Check Hugging Face URL and network connectivity

3. **Conversion failed**
   - Error: "Conversion completed but output files not found"
   - Solution: Check pnnx output, model may not be compatible

4. **Model not compatible**
   - Some ONNX models may not convert successfully
   - Check pnnx documentation for supported operators

## Technical Details

### Conversion Endpoint
- **File**: `pgstar/ai/convert.c`
- **Function**: `pgstar_route_v1_models_convert()`
- **Dependencies**: pnnx, curl, sys/wait.h for process management

### Chat Endpoint Updates
- **File**: `pgstar/ai/chat.c`
- **Change**: Added ONNX model fallback loading
- **Function**: `c_ncnn_load_onnx_model()` from c_ncnn library

### Models UI Updates
- **File**: `pgstar/ai/models_ui.c`
- **Change**: Dual search (NCNN → ONNX)
- **Feature**: Convert button for ONNX models

## Future Enhancements

- Direct ONNX inference support (without conversion)
- Batch conversion support
- Conversion progress tracking
- Model validation before conversion
- Support for other model formats (PyTorch, TensorFlow)

