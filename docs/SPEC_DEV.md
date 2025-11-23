# pgstar Development Notes

## UI page rendering

- `pgstar_ui_render_page()` returns a freshly allocated buffer for the HTML body.  
  When wiring that into an HTTP response:
  - Copy it with `pstrdup()` for any long-lived owner (response body, cache entry, etc.).
  - `pfree()` the original buffer once copies are made so we do not leak memory.
  - Never hand the original pointer to multiple owners without copying, otherwise the HTTP layer may free it while another component still references it.
- `PgstarResponse` objects are freed by `pgstar_http_request_handle()` (both `resp.body` and `resp.log`). Treat anything stored there as owned by the HTTP layer.

## Safe Memory Management

- **Always use `pgstar_safe_pfree()` instead of `pfree()`** to prevent segfaults from NULL pointers, double-free, or invalid memory. The safe wrapper logs warnings at INFO level for debugging.

- **Free temporary allocations after copying**: When you `pstrdup()` a value for a caller, free the original buffer immediately after copying.

- **Track conditional allocations**: If you conditionally allocate memory (e.g., `pstrdup()` only when NULL), use a flag to track whether you allocated it, then free only what you allocated.

- **SPI memory context**: Values from `SPI_getvalue()` are managed by SPI's memory context and will be freed when `SPI_finish()` is called. Don't free them manually unless you've copied them first.

- **StringInfo buffers**: After copying `buf.data` with `pstrdup()`, always free `buf.data` with `pgstar_safe_pfree()` since it's allocated in your current memory context.

### Progressive Examples

**Example 1: Basic safe free**
```c
char *ptr = palloc(100);
// ... use ptr ...
pgstar_safe_pfree(ptr);  // Safe even if ptr is NULL or invalid
```

**Example 2: Free after copying**
```c
char *temp = pstrdup("some value");
*output = pstrdup(temp);  // Caller gets a copy
pgstar_safe_pfree(temp);  // Free original immediately
```

**Example 3: StringInfo cleanup**
```c
StringInfoData buf;
initStringInfo(&buf);
appendStringInfoString(&buf, content);
*body = pstrdup(buf.data);  // Copy for caller
pgstar_safe_pfree(buf.data);  // Free StringInfo buffer
```

**Example 4: Conditional allocation tracking**
```c
char *ct = SPI_getvalue(tup, tupdesc, 2);
bool ct_allocated = false;

if (ct == NULL) {
    ct = pstrdup("text/html");
    ct_allocated = true;  // Track that we allocated it
}

*content_type = pstrdup(ct);  // Copy for caller

if (ct_allocated)
    pgstar_safe_pfree(ct);  // Free only if we allocated it
// Note: SPI_getvalue results are freed by SPI_finish()
```

**Example 5: Complex cleanup pattern**
```c
StringInfoData buf;
char *content = SPI_getvalue(tup, tupdesc, 1);
char *ct = SPI_getvalue(tup, tupdesc, 2);
bool ct_allocated = false;

if (ct == NULL) {
    ct = pstrdup("application/json");
    ct_allocated = true;
}

initStringInfo(&buf);
appendStringInfoString(&buf, content);
*body = pstrdup(buf.data);
*content_type = pstrdup(ct);

// Cleanup: free what we allocated
pgstar_safe_pfree(buf.data);
if (ct_allocated)
    pgstar_safe_pfree(ct);
// content and ct (if from SPI) are freed by SPI_finish()
```

**Example 6: SPI memory context with error handling (Beautiful PostgreSQL Pattern)**

This example demonstrates how PostgreSQL's SPI memory context works beautifully with error handling. `SPI_getvalue()` results are automatically freed by `SPI_finish()`, which must be called in both success and error paths:

```c
static bool
pgstar_ui_fetch_asset(const char *kind, const char *name,
                      char **body, const char **content_type)
{
    StringInfoData buf;
    int         ret;
    bool        found = false;
    bool        ct_allocated = false;  // Track if we allocated ct ourselves
    char       *content;
    char       *ct;

    SPI_connect();
    
    PG_TRY();
    {
        // ... SPI query execution ...
        
        content = SPI_getvalue(tup, tupdesc, 1);  // SPI-managed memory
        ct = SPI_getvalue(tup, tupdesc, 2);       // SPI-managed memory
        
        if (ct == NULL) {
            // We allocate this ourselves, so we must free it
            ct = pstrdup("text/css; charset=utf-8");
            ct_allocated = true;  // Mark that we own this allocation
        }
        
        initStringInfo(&buf);
        appendStringInfoString(&buf, content);
        
        *body = pstrdup(buf.data);        // Copy for caller
        *content_type = pstrdup(ct);      // Copy for caller
        
        // Cleanup: free only what WE allocated
        pgstar_safe_pfree(buf.data);      // We allocated this StringInfo buffer
        if (ct_allocated)
            pgstar_safe_pfree(ct);        // We allocated this, so we free it
        // content and ct (if from SPI_getvalue) are NOT freed here -
        // they will be freed by SPI_finish() below
        
        found = true;
    }
    PG_CATCH();
    {
        FlushErrorState();
        SPI_finish();  // Must call SPI_finish() in error path too!
        // This frees all SPI_getvalue() results (content, ct if from SPI)
        PopActiveSnapshot();
        CommitTransactionCommand();
        return false;
    }
    PG_END_TRY();
    
    SPI_finish();  // Must call SPI_finish() in success path too!
    // This frees all SPI_getvalue() results (content, ct if from SPI)
    PopActiveSnapshot();
    CommitTransactionCommand();
    
    return found;
}
```

**Key points:**
- `SPI_getvalue()` returns pointers to memory in SPI's memory context
- This memory is automatically freed when `SPI_finish()` is called
- `SPI_finish()` **must** be called in both `PG_CATCH()` and after `PG_END_TRY()` to ensure cleanup
- We only free memory we allocated ourselves (tracked with `ct_allocated` flag)
- This is a beautiful PostgreSQL pattern: SPI manages its own memory context, and we only manage what we allocate


