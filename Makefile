DEFAULT_EXT_DIRS := hello_world pgstar

ifeq ($(strip $(EXT_DIRS)),)
ifdef EXT_DIR
EXT_DIRS := $(EXT_DIR)
else
EXT_DIRS := $(DEFAULT_EXT_DIRS)
endif
endif

EXT_BUILD_TARGETS := $(addprefix extension-build-,$(EXT_DIRS))
EXT_INSTALL_TARGETS := $(addprefix extension-install-,$(EXT_DIRS))
EXT_CLEAN_TARGETS := $(addprefix extension-clean-,$(EXT_DIRS))
EXT_UNINSTALL_TARGETS := $(addprefix extension-uninstall-,$(EXT_DIRS))

.PHONY: help test extension-build extension-install extension-clean extension-uninstall \
	extension-build-% extension-install-% extension-clean-% extension-uninstall-%

help:
	@echo "Available targets:"
	@echo "  make test                 # Build all extensions (safe to run with '&')"
	@echo "  make extension-build      # Build extensions listed in EXT_DIRS (default: $(DEFAULT_EXT_DIRS))"
	@echo "  make extension-install    # Install the extensions via pg_config/PGXS"
	@echo "  make extension-clean      # Clean build artifacts"
	@echo "  make extension-uninstall  # Remove the installed extension binaries"
	@echo ""
	@echo "Use EXT_DIRS=\"pgstar\" (or legacy EXT_DIR=pgstar) to scope the targets."

test: extension-build
clean:
	mkdir -p bin/
	make extension-clean

extension-build: $(EXT_BUILD_TARGETS)

extension-install: $(EXT_INSTALL_TARGETS)

extension-clean: $(EXT_CLEAN_TARGETS)

extension-uninstall: $(EXT_UNINSTALL_TARGETS)

extension-build-%:
	$(MAKE) -C $*

extension-install-%: extension-build-%
	$(MAKE) -C $* install

extension-clean-%:
	$(MAKE) -C $* clean

extension-uninstall-%:
	$(MAKE) -C $* uninstall

all: install

build: extension-clean-pgstar extension-build-pgstar

install: extension-install-pgstar

# auto: auto-build auto-run

build-install-run: build install run

auto-build-run:
	find . -name "*.c" -o -name "*.sql"  -o -name ".env" | entr -s 'make build-install-run'

auto-run:
	find . -maxdepth 1 -name "run.sql" | entr -s 'make run'

auto: auto-build-run

run:
	PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -f ./run.sql

run1:
	sudo -u postgres psql -f ./run.sql

psql:
	PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost

psql2:
	sudo -u postgres psql
