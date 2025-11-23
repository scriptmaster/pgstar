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
	extension-build-% extension-install-% extension-clean-% extension-uninstall-% \
	git-status git-checkout-main git-merge-pgstar_http_post git-merge-pgstar_jobs git-commit \
	git-new-branch-pgstar-http-post pgstar-symbols run-locations-sync2025

help:
	@echo "Available targets:"
	@echo "  make test                 # Build all extensions (safe to run with '&')"
	@echo "  make extension-build      # Build extensions listed in EXT_DIRS (default: $(DEFAULT_EXT_DIRS))"
	@echo "  make extension-install    # Install the extensions via pg_config/PGXS"
	@echo "  make extension-clean      # Clean build artifacts"
	@echo "  make extension-uninstall  # Remove the installed extension binaries"
	@echo "  make run-locations-sync2025  # Configure FDW + register telemetry->locations2025 job"
	@echo ""
	@echo "Use EXT_DIRS=\"pgstar\" (or legacy EXT_DIR=pgstar) to scope the targets."

test: extension-build
clean: extension-clean

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

restart:
	systemctl restart postgresql

build-install-run: build install restart run

auto-build-run:
	find . -name "*.c" -o -name "*.sql"  -o -name ".env" | entr -s 'make build-install-run'

auto-run:
	find . -maxdepth 1 -name "run.sql" | entr -s 'make run'

auto: auto-build-run

run:
	PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -f ./run.sql

run1:
	sudo -u postgres psql -f ./run.sql

run-locations-sync2025:
	PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -f ./start_locations_sync2025.sql

psql:
	PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost

psql2:
	sudo -u postgres psql

git-status:
	git status -b

git-checkout-main:
	git checkout main

git-merge-pgstar_http_post:
	git merge pgstar_http_post

git-merge-pgstar_jobs:
	git merge pgstar_jobs

git-commit:
	git add -A
	git commit -m "chore: pgstar http, jobs, users, and build wiring"

git-new-branch-pgstar-http-post:
	git checkout -b pgstar_http_post

pgstar-symbols:
	nm -D /usr/lib/postgresql/14/lib/pgstar.so | grep pgstar_http_post || true
