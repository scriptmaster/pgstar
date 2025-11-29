EXTENSION = pgstar
MODULE_big = pgstar
PGSTAR_VERSION := $(shell sed -n "s/^default_version[[:space:]]*=[[:space:]]*'\\([^']*\\)'/\\1/p" pgstar.control)
GENERATED_DIR := generated
VERSION_HEADER := $(GENERATED_DIR)/pgstar_version.h
SQL_VERSIONED := $(EXTENSION)--$(PGSTAR_VERSION).sql
ARCHIVE_DIR := $(GENERATED_DIR)/archive

.DEFAULT_GOAL := all

PG_CPPFLAGS += -I$(GENERATED_DIR) -Icore -Iui

# Check for c_ncnn library
C_NCNN_LIB := ../c_ncnn/build/libc_ncnn.so
C_NCNN_INC := ../c_ncnn/include

# Add c_ncnn support if available
ifeq ($(wildcard ../c_ncnn/Makefile),)
    # c_ncnn directory doesn't exist, build without it
    $(info c_ncnn not found, building pgstar without AI support)
else
    # Try to build c_ncnn if it doesn't exist
    ifeq ($(wildcard $(C_NCNN_LIB)),)
        $(info Building c_ncnn library...)
        $(shell $(MAKE) -C ../c_ncnn all 2>/dev/null || true)
    endif
    
    # Add c_ncnn support if library exists
    ifeq ($(wildcard $(C_NCNN_LIB)),)
        $(warning c_ncnn library not found after build attempt, continuing without it)
    else
        $(info Using c_ncnn library: $(C_NCNN_LIB))
        PG_CPPFLAGS += -DHAVE_C_NCNN -I$(C_NCNN_INC)
        # Link against c_ncnn - use system library path or copy to PostgreSQL lib dir
        SHLIB_LINK += -L../c_ncnn/build -lc_ncnn -Wl,-rpath,/usr/lib/postgresql/14/lib:/usr/local/lib
    endif
endif

# OpenSSL support for HTTPS downloads
SHLIB_LINK += -lssl -lcrypto

UI_DIR := ui
UI_ROOT_HTML := $(UI_DIR)/root.html
UI_ROOT_C := $(UI_DIR)/root_page.c

# Automatically include all C files in the extension tree
SRCS := $(wildcard *.c core/*.c api/*.c tcp/*.c tcp/protocols/*.c http/*.c functions/*.c ui/*.c ai/*.c)
OBJS := $(SRCS:.c=.o)
DATA = $(SQL_VERSIONED)
PGFILEDESC = "pgstar - mock server helpers and version reporting"

$(GENERATED_DIR):
	@mkdir -p $@

$(ARCHIVE_DIR): | $(GENERATED_DIR)
	@mkdir -p $@

$(VERSION_HEADER): pgstar.control $(SQL_VERSIONED) | $(GENERATED_DIR)
	@echo "#ifndef PGSTAR_VERSION_STRING" > $@
	@echo "#define PGSTAR_VERSION_STRING \"$(PGSTAR_VERSION)\"" >> $@
	@echo "#endif" >> $@

# Ensure the extension install script filename matches default_version
# declared in pgstar.control (e.g., pgstar--1.15.sql).
# Order:
#  1) If pgstar--$(PGSTAR_VERSION).sql does NOT exist yet, create it
#     from the first matching pgstar--*.sql in the project root.
#  2) Then move ALL other pgstar--*.sql files to ./generated/archive/,
#     overwriting any existing archived versions.
$(SQL_VERSIONED): pgstar.control | $(ARCHIVE_DIR)
	@if [ ! -f "$(SQL_VERSIONED)" ]; then \
		src=$$(ls pgstar--*.sql 2>/dev/null | head -n 1); \
		if [ -z "$$src" ]; then \
			echo "ERROR: no pgstar--*.sql found to seed $(SQL_VERSIONED)" >&2; \
			exit 1; \
		fi; \
		echo "Creating $(SQL_VERSIONED) from $$src"; \
		cp "$$src" "$(SQL_VERSIONED)"; \
	fi
	@for f in pgstar--*.sql; do \
		if [ "$$f" != "$(SQL_VERSIONED)" ]; then \
			echo "Archiving $$f to $(ARCHIVE_DIR)/$$f"; \
			mv -f "$$f" "$(ARCHIVE_DIR)/$$f"; \
		fi; \
	done

$(OBJS): $(VERSION_HEADER) $(UI_ROOT_C)

$(UI_ROOT_C): $(UI_ROOT_HTML)
	@echo "Generating $@ from $<"
	@printf '#include "postgres.h"\n\n' > $@
	@printf 'const char *pgstar_root_html =\n' >> $@
	@sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< >> $@
	@echo ';' >> $@

all: $(VERSION_HEADER)

EXTRA_CLEAN += $(VERSION_HEADER)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
