EXTENSION = pgstar
DATA = pgstar--1.31.sql
PGFILEDESC = "pgstar - postgres web server"

MODULE_big = pgstar
OBJS = main.o \
       core/env.o core/jobs_worker.o core/pgstar.o core/signals.o core/spi.o core/stat.o core/utils.o core/worker.o \
       api/files.o api/information_schema.o \
       tcp/protocol.o tcp/socket.o tcp/protocols/other.o \
       http/ai.o http/api.o http/client.o http/request.o http/response.o \
       http/route_pgstar.o http/router.o http/static.o http/uri.o http/v1.o \
       functions/pgstar_http_get.o functions/pgstar_http_post.o functions/pgstar_jobs.o \
       ui/css_links.o ui/js_scripts.o ui/meta_tags.o ui/pages.o ui/pages_layout.o ui/root_page.o \
       ai/chat.o ai/convert.o ai/models_local.o ai/models_ui.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

# C_NCNN settings
C_NCNN_DIR = ../c_ncnn
C_NCNN_LIB = $(C_NCNN_DIR)/build/libc_ncnn.so
C_NCNN_INCLUDE = $(C_NCNN_DIR)/include

# Add OpenSSL flags
SHLIB_LINK += -lssl -lcrypto

# Add C_NCNN include path and macro
PG_CPPFLAGS += -Igenerated -Icore -Iui -DHAVE_C_NCNN -I$(C_NCNN_INCLUDE) -I. -I./

# Add C_NCNN library to linker flags
SHLIB_LINK += -L$(C_NCNN_DIR)/build -lc_ncnn -Wl,-rpath,/usr/lib/postgresql/14/lib:/usr/local/lib

include $(PGXS)

# Ensure C_NCNN library exists before building extension
$(OBJS): $(C_NCNN_LIB)

$(C_NCNN_LIB):
	@echo "Building c_ncnn library first..."
	$(MAKE) -C $(C_NCNN_DIR)

# Custom install target to ensure C_NCNN lib is available
install: install-c-ncnn

install-c-ncnn:
	@echo "Using c_ncnn library: $(C_NCNN_LIB)"
