EXTENSION = pgstar
MODULE_big = pgstar
PGSTAR_VERSION := $(shell sed -n "s/^default_version[[:space:]]*=[[:space:]]*'\\([^']*\\)'/\\1/p" pgstar.control)
GENERATED_DIR := generated
VERSION_HEADER := $(GENERATED_DIR)/pgstar_version.h

.DEFAULT_GOAL := all

PG_CPPFLAGS += -I$(GENERATED_DIR) -Icore

OBJS = main.o \
	core/pgstar.o \
	core/env.o \
	core/stat.o \
	core/spi.o \
	core/signals.o \
	core/worker.o \
	api/information_schema.o \
	tcp/socket.o \
	tcp/protocol.o \
	tcp/protocols/other.o \
	http/request.o \
	http/response.o \
	http/router.o
DATA = pgstar--1.0.sql
PGFILEDESC = "pgstar - mock server helpers and version reporting"

$(GENERATED_DIR):
	@mkdir -p $@

$(VERSION_HEADER): pgstar.control | $(GENERATED_DIR)
	@echo "#ifndef PGSTAR_VERSION_STRING" > $@
	@echo "#define PGSTAR_VERSION_STRING \"$(PGSTAR_VERSION)\"" >> $@
	@echo "#endif" >> $@

$(OBJS): $(VERSION_HEADER)

all: $(VERSION_HEADER)

EXTRA_CLEAN += $(VERSION_HEADER)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

