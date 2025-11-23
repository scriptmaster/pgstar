EXTENSION = pgstar
MODULE_big = pgstar
OBJS = main.o \
	core/env.o \
	core/stat.o \
	core/spi.o \
	core/signals.o \
	core/worker.o \
	functions/pgstar_http_post.o \
	functions/pgstar_http_get.o \
	functions/pgstar_jobs.o \
	tcp/socket.o \
	tcp/protocol.o \
	tcp/protocols/other.o \
	http/request.o
DATA = pgstar--1.0.sql
PGFILEDESC = "pgstar - mock server helpers and version reporting"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
