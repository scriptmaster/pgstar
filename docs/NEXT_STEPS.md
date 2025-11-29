# Next Steps - Testing the AI Endpoint

## ✅ What's Done

1. ✅ Built `c_ncnn` library with custom HTTP client
2. ✅ Built `pgstar` extension with AI endpoint support
3. ✅ Created `/v1/chat/completions` endpoint

## 📋 Next Steps

### 1. Install the Extension

```bash
cd /root/workspace/pg
make extension-install-pgstar
```

Or manually:
```bash
cd pgstar
make install
```

### 2. Configure PostgreSQL

Ensure `postgresql.conf` has:
```conf
shared_preload_libraries = 'pgstar'
pgstar.port = 5481
pgstar.log_level = 'info'
```

Then restart PostgreSQL:
```bash
sudo systemctl restart postgresql
# or
make restart
```

### 3. Create the Extension in Database

```bash
psql -U postgres -d your_database
```

Then:
```sql
CREATE EXTENSION IF NOT EXISTS pgstar;
```

### 4. Test the Endpoint (Without Model)

The endpoint will work even without a model (returns placeholder):

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

Expected response:
```json
{
  "id": "chatcmpl-pgstar-...",
  "object": "chat.completion",
  "model": "pgstar-ncnn",
  "choices": [{
    "message": {
      "role": "assistant",
      "content": "Hello! Model not loaded, but endpoint is working..."
    }
  }]
}
```

### 5. (Optional) Add a Model for Testing

To test with an actual model, you need NCNN model files:

```bash
# Place model files in /opt/pgstar/models/
mkdir -p /opt/pgstar/models
cp your_model.param /opt/pgstar/models/pgstar_model.param
cp your_model.bin /opt/pgstar/models/pgstar_model.bin
```

Or use the test script:
```bash
cd c_ncnn
./test_model.sh
mkdir -p /opt/pgstar/models
cp /tmp/pgstar_models/test_model.* /opt/pgstar/models/pgstar_model.*
```

**Note**: The current implementation uses stub code. For real inference, you'll need to:
- Install NCNN library
- Rebuild c_ncnn with NCNN support
- Provide actual model files

### 6. Check Logs

Monitor PostgreSQL logs to see endpoint activity:

```bash
make log
# or
tail -f /var/lib/postgresql/14/main/log/*.log
```

## 🎯 Quick Test Commands

```bash
# Test health endpoint
curl http://localhost:5481/

# Test version endpoint
curl http://localhost:5481/pgstar/version

# Test AI endpoint
curl -X POST http://localhost:5481/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"test"}]}'
```

## 📝 Troubleshooting

### Extension not loading?
- Check `shared_preload_libraries` in postgresql.conf
- Restart PostgreSQL
- Check logs: `make log`

### Endpoint returns 404?
- Verify pgstar is running: `SELECT pgstar_version();`
- Check port: `netstat -tlnp | grep 5481`
- Check logs for routing errors

### Model not loading?
- Check file permissions: `ls -lh /opt/pgstar/models/pgstar_model.*`
- Check logs for model loading errors
- Current implementation uses stub - real models need NCNN library

## 🔗 Related Files

- Endpoint handler: `pgstar/ai/chat.c`
- Router: `pgstar/http/v1.c`
- Library: `c_ncnn/build/libc_ncnn.so`
- Documentation: `docs/AI_ENDPOINT.md`

