# Installing libc_ncnn.so for pgstar

## Problem

PostgreSQL can't find `libc_ncnn.so` when loading the `pgstar` extension because the library isn't in the system library search path.

## Solution

Copy `libc_ncnn.so` to a location where PostgreSQL can find it:

### Option 1: Copy to PostgreSQL lib directory (Recommended)

```bash
sudo cp c_ncnn/build/libc_ncnn.so /usr/lib/postgresql/14/lib/
```

This is the simplest solution - PostgreSQL will find it in the same directory as `pgstar.so`.

### Option 2: Install to system library directory

```bash
sudo cp c_ncnn/build/libc_ncnn.so /usr/local/lib/
sudo ldconfig
```

Then rebuild pgstar with rpath pointing to `/usr/local/lib`.

### Option 3: Update Makefile to copy during install

Add to `pgstar/Makefile`:

```makefile
install: all
    # ... existing install commands ...
    cp ../c_ncnn/build/libc_ncnn.so $(DESTDIR)$(pkglibdir)/
```

## Verification

After copying the library, verify it's found:

```bash
ldd /usr/lib/postgresql/14/lib/pgstar.so | grep c_ncnn
```

Should show:
```
libc_ncnn.so => /usr/lib/postgresql/14/lib/libc_ncnn.so
```

Then restart PostgreSQL and test:

```bash
sudo systemctl restart postgresql
sudo -u postgres psql -c "SELECT pgstar_version();"
```

## Current Status

✅ Library copied to `/usr/lib/postgresql/14/lib/libc_ncnn.so`
✅ Extension rebuilt with correct rpath
✅ PostgreSQL can now load the extension

