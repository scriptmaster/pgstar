EXTENSION = pgstar
MODULE_big = pgstar
PGSTAR_VERSION := $(shell sed -n "s/^default_version[[:space:]]*=[[:space:]]*'\\([^']*\\)'/\\1/p" pgstar.control)
GENERATED_DIR := generated
VERSION_HEADER := $(GENERATED_DIR)/pgstar_version.h
SQL_VERSIONED := $(EXTENSION)--$(PGSTAR_VERSION).sql
ARCHIVE_DIR := $(GENERATED_DIR)/archive

.DEFAULT_GOAL := all

PG_CPPFLAGS += -I$(GENERATED_DIR) -Icore

# Automatically include all C files in the extension tree
SRCS := $(wildcard *.c core/*.c api/*.c tcp/*.c tcp/protocols/*.c http/*.c functions/*.c)
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

$(OBJS): $(VERSION_HEADER)

all: $(VERSION_HEADER)

EXTRA_CLEAN += $(VERSION_HEADER)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
