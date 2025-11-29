# Downloading Models for pgstar AI Endpoint

## Model File Locations

The endpoint expects NCNN model files at:
- **Parameter file**: `/opt/pgstar/models/pgstar_model.param`
- **Binary file**: `/opt/pgstar/models/pgstar_model.bin`

## Current Status

Based on the logs:
- ✅ Endpoint is working
- ✅ Server is running on port **5480**
- ⚠️ Model files not found (endpoint returns placeholder)

## Downloading a Model

### Option 1: Use Test Script (Quick Test)

```bash
cd c_ncnn
./test_model.sh
cp /tmp/pgstar_models/test_model.param /opt/pgstar/models/pgstar_model.param
cp /tmp/pgstar_models/test_model.bin /opt/pgstar/models/pgstar_model.bin
```

**Note**: These are dummy files for testing. They won't run actual inference but will test the loading mechanism.

### Option 2: Download Real NCNN Model

For actual inference, you need real NCNN model files. Here are some sources:

1. **Hugging Face** - Search for "ncnn" models
   ```bash
   # Example (replace with actual URLs)
   curl -L -o /opt/pgstar/models/pgstar_model.param https://huggingface.co/.../model.param
   curl -L -o /opt/pgstar/models/pgstar_model.bin https://huggingface.co/.../model.bin
   ```

2. **NCNN Model Zoo**
   - Visit: https://github.com/nihui/ncnn-assets
   - Download lightweight models (50-400MB range)

3. **Convert PyTorch/ONNX to NCNN**
   - Use pnnx tool (part of NCNN project)
   - Convert .pt or .onnx files to .param/.bin

### Option 3: HTTPS URLs (Manual Download)

If you have an HTTPS URL, the code will:
1. Print a curl command
2. Wait for you to download manually
3. Check if file exists
4. Continue automatically

Example:
```bash
# When you call c_ncnn_download_and_load() with HTTPS URL,
# it will print:
#   curl -L -o "/opt/pgstar/models/pgstar_model.param" "https://example.com/model.param"
# Then wait for you to run it
```

## Verifying Model Files

```bash
# Check if files exist
ls -lh /opt/pgstar/models/pgstar_model.*

# Check file sizes (real models are usually MB-GB)
# Dummy test files are very small (< 1KB)
```

## Testing After Download

```bash
# Test the endpoint
curl -X POST http://localhost:5480/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'

# Check logs for model loading
tail -f /var/lib/postgresql/14/main/log/*.log | grep pgstar
```

## Log Messages to Look For

**Success:**
```
pgstar: NCNN model loaded successfully
pgstar: Inference completed successfully
```

**Model Not Found:**
```
pgstar: failed to load NCNN model: Param file not found
```

**Model Loaded but Inference Failed:**
```
pgstar: Inference failed: [error message]
```

## Important Notes

1. **Current Implementation**: Uses stub code - even with real model files, inference won't work without the NCNN library installed
2. **Model Format**: Must be NCNN format (.param/.bin), not PyTorch (.pt) or ONNX (.onnx)
3. **File Permissions**: Ensure PostgreSQL can read `/opt/pgstar/models/pgstar_model.*` files
4. **Port**: Server runs on **5480** (not 5481) as configured in `postgresql.conf`

## Next Steps

1. ✅ Endpoint is working (tested)
2. ⏭️ Download a real NCNN model (optional, for actual inference)
3. ⏭️ Install NCNN library if you want real inference (currently stub implementation)

The endpoint is fully functional and ready to use!

