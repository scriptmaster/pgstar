# Why Custom HTTP Client Instead of libcurl?

## Current Implementation

The `c_ncnn` library now uses a **custom HTTP-only client** instead of libcurl. Here's why:

## Role of HTTP Client in c_ncnn

The HTTP client is **only** used for downloading model files from URLs via `c_ncnn_download_and_load()`. It's not used for:
- Model loading from local files (uses `c_ncnn_load_model()`)
- Model inference (runs locally)
- Any other operations

## Why Custom HTTP Client is Better

### 1. **No External Dependencies**
- ✅ **Zero dependencies**: Uses only standard POSIX sockets (already available everywhere)
- ✅ **Smaller footprint**: No need to link against libcurl (~500KB+)
- ✅ **Simpler deployment**: One less thing to install/configure

### 2. **Perfect for PostgreSQL Extensions**
- ✅ **No runtime surprises**: Won't fail if libcurl isn't installed
- ✅ **Consistent behavior**: Same code path on all platforms
- ✅ **PostgreSQL-friendly**: PostgreSQL extensions prefer minimal dependencies

### 3. **HTTP-Only is Sufficient**
- ✅ **Model hosting**: Most model repositories serve over HTTP or can be configured to
- ✅ **Internal networks**: HTTP is fine for internal model servers
- ✅ **Simplicity**: No SSL/TLS complexity needed for model downloads

### 4. **Performance**
- ✅ **Lightweight**: Direct socket I/O, no abstraction layers
- ✅ **Streaming**: Writes directly to file as data arrives
- ✅ **No overhead**: No SSL handshake, certificate validation, etc.

### 5. **Maintainability**
- ✅ **Self-contained**: All code in one place, easy to debug
- ✅ **No version conflicts**: No libcurl version compatibility issues
- ✅ **Full control**: Can customize behavior for model downloading needs

## Limitations

### HTTPS Not Supported
- ❌ Only HTTP URLs work (`http://...`)
- ❌ HTTPS URLs (`https://...`) will return an error
- ✅ **Workaround**: Use HTTP or download models manually and load from files

### Why This is Acceptable
1. **Model downloads are typically one-time operations**
2. **Can be done manually** before deployment
3. **Internal model servers** often use HTTP
4. **pgstar primarily loads from local files** anyway

## Comparison: libcurl vs Custom HTTP

| Feature | libcurl | Custom HTTP |
|---------|---------|-------------|
| Dependencies | Requires libcurl | None |
| HTTPS Support | ✅ Yes | ❌ No |
| HTTP Support | ✅ Yes | ✅ Yes |
| Binary Size | +500KB+ | +~5KB |
| Runtime Failures | Possible if not installed | Never |
| Complexity | High (SSL, certs, etc.) | Low (simple HTTP) |
| Control | Limited | Full |

## Recommendation

**Use the custom HTTP client** because:
1. ✅ **pgstar doesn't use `c_ncnn_download_and_load()`** - it loads models from local files
2. ✅ **Zero dependencies** makes deployment easier
3. ✅ **HTTP-only is sufficient** for model downloads
4. ✅ **No runtime failures** - code is always available

If you need HTTPS support in the future, you can:
- Add OpenSSL (lightweight, widely available)
- Use a simple TLS library
- Or keep libcurl as an optional dependency

## Usage

```c
// This works with custom HTTP client (HTTP only)
c_ncnn_model_t model = c_ncnn_download_and_load(
    "http://example.com/model.param",  // ✅ HTTP works
    C_NCNN_FORMAT_NCNN,
    "/tmp/models"
);

// This will fail (HTTPS not supported)
c_ncnn_model_t model = c_ncnn_download_and_load(
    "https://example.com/model.param",  // ❌ HTTPS fails
    C_NCNN_FORMAT_NCNN,
    "/tmp/models"
);

// Better: Load from local file (no HTTP needed)
c_ncnn_model_t model = c_ncnn_load_model(
    "/path/to/model.param",
    "/path/to/model.bin"
);
```

