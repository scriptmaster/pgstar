EXTENSION = pgstar
MODULE_big = pgstar
OBJS = main.o \
	pgstar.o \
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

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

