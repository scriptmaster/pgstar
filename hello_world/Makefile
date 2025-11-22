EXTENSION = hello_world
MODULE_big = hello_world
OBJS = hello_world.o
DATA = hello_world--1.0.sql
PGFILEDESC = "hello_world - minimal Postgres C extension returning a greeting"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

