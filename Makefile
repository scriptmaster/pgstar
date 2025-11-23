DEFAULT_EXT_DIRS := hello_world pgstar
AUTO_RUN_DEBOUNCE ?= 3

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
	git-new-branch-pgstar-http-post git-new-branch-exec-jobs git-commit-exec-jobs \
	pgstar-symbols run-locations-sync2025 log logs log-follow

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
	@find . -name "*.c" -o -name "*.sql"  -o -name ".env" | entr -s ' \
		DEBOUNCE_FILE="/tmp/make-auto-debounce"; \
		DEBOUNCE_SEC=$(AUTO_RUN_DEBOUNCE); \
		NOW=$$(date +%s); \
		if [ -f "$$DEBOUNCE_FILE" ]; then \
			LAST=$$(cat "$$DEBOUNCE_FILE" 2>/dev/null || echo 0); \
			ELAPSED=$$((NOW - LAST)); \
			if [ $$ELAPSED -lt $$DEBOUNCE_SEC ]; then \
				SLEEP_TIME=$$((DEBOUNCE_SEC - ELAPSED)); \
				sleep $$SLEEP_TIME; \
				NOW=$$(date +%s); \
			fi; \
		fi; \
		echo $$NOW > "$$DEBOUNCE_FILE"; \
		make build-install-run'

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

log logs:
	@echo "Showing recent PostgreSQL logs (last 200 lines)..."
	sudo journalctl -u postgresql --no-pager -n 200
	sleep 1
	@echo "Tailing latest PostgreSQL log file under /var/lib/postgresql/14/main/log (Ctrl+C to stop)..."
	@sudo sh -c 'latest=$$(ls -1t /var/lib/postgresql/14/main/log/*.log 2>/dev/null | head -n 1); \
		if [ -z "$$latest" ]; then \
			echo "No PostgreSQL log files found in /var/lib/postgresql/14/main/log"; \
			exit 1; \
		fi; \
		echo "Following $$latest"; \
		tail -f "$$latest"'

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

git-new-branch-exec-jobs:
	git checkout -b exec_jobs

git-commit-exec-jobs:
	git add -A
	git commit -m "feat: add pgstar jobs executor worker"

pgstar-symbols:
	nm -D /usr/lib/postgresql/14/lib/pgstar.so | grep pgstar_http_post || true
