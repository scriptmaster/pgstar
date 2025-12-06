.PHONY: extension-install extension-clean restart test run auto auto-run log logs tail psql e2e print-pwd fetch-assets bump

# Default target
all: auto

install: extension-install

# Extension management
extension-install:
	@make -s -C hello_world
	@make -s -C hello_world install
	@make -s -C pgstar
	@make -s -C pgstar install
	@make restart
	@make create-extension
	@make restart

# Server management
restart:
	@sudo systemctl restart postgresql

# Testing and Running
test: extension-install restart create-extension
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -c "SELECT 1" > /dev/null

create-extension:
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -c "DROP EXTENSION IF EXISTS pgstar CASCADE; CREATE EXTENSION pgstar;"

run: extension-install
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -f ./run.sql
	@make restart

# Logging
logf:
	@echo "Tailing latest PostgreSQL log file under /var/lib/postgresql/14/main/log (Ctrl+C to stop)..."
	@sudo sh -c 'latest=$$(ls -1t /var/lib/postgresql/14/main/log/*.log 2>/dev/null | head -n 1); \
		if [ -z "$$latest" ]; then \
			echo "No PostgreSQL log files found in /var/lib/postgresql/14/main/log"; \
			exit 1; \
		fi; \
		echo "Following $$latest"; \
		tail -f "$$latest"'

log tail:
	@echo "Showing recent PostgreSQL logs (last 200 lines)..."
	@sudo sh -c 'latest=$$(ls -1t /var/lib/postgresql/14/main/log/*.log 2>/dev/null | head -n 1); \
		if [ -z "$$latest" ]; then \
			echo "No PostgreSQL log files found in /var/lib/postgresql/14/main/log"; \
			exit 1; \
		fi; \
		 echo .; echo $$latest; echo .; echo .; \
		tail -200 "$$latest"'

# Auto-run (Build, Install, Restart, Run, Log)
auto: auto-run

auto-run:
	@while true; do \
		make run || true; \
		echo "Starting log tail (Ctrl+C to restart auto-run)..."; \
		make logf || true; \
		echo "Log tail ended, restarting auto-run..."; \
		sleep 1; \
	done

# Direct PSQL access
psql:
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost

# Debug helper
print-pwd:
	@pwd
# Simple stability smoke tests for critical endpoints
e2e:
	@echo "Running curl stability checks against /api/files and /api/pgstar/users..."
	@bash -c 'set -euo pipefail; \
	rm -f e2e.out; \
	for endpoint in /api/files /api/pgstar/users ; do \
		printf "Endpoint: %s\n" "$$endpoint" | tee -a e2e.out; \
		for i in 1 2 3 4 5 6 7 ; do \
			printf "  Request %d -> %s: " "$$i" "$$endpoint" | tee -a e2e.out; \
			if ! curl -s -w " [status:%{http_code} time:%{time_total}s]\n" "http://127.0.0.1:5480$$endpoint" | tee -a e2e.out; then \
				echo "  curl failed for $$endpoint (request $$i)" | tee -a e2e.out; \
			fi; \
			sleep 2; \
		done; \
	done; \
	echo "--- Aggregated Output ---"; \
	cat e2e.out'

fetch-assets:
	@echo "Fetching static assets to exercise pgstar_ui_fetch_file..."
	@curl -s -w 'vue.global.prod.min.js [status:%{http_code} time:%{time_total}s]\n' -o /tmp/vue.global.prod.min.js "http://127.0.0.1:5480/static/js/vue.global.prod.min.js"
	@curl -s -w 'vue-data-table.js [status:%{http_code} time:%{time_total}s]\n' -o /tmp/vue-data-table.js "http://127.0.0.1:5480/static/js/vue-data-table.js"
	@curl -s -w 'favicon.ico [status:%{http_code} time:%{time_total}s]\n' -o /tmp/favicon.ico "http://127.0.0.1:5480/favicon.ico"

# Version bump automation
bump:
	@bash -c 'set -euo pipefail; \
	PGSTAR_DIR="pgstar"; \
	CONTROL_FILE="$$PGSTAR_DIR/pgstar.control"; \
	MAKEFILE="$$PGSTAR_DIR/Makefile"; \
	\
	# Find current SQL file and version \
	CURRENT_SQL=$$(ls -1 $$PGSTAR_DIR/pgstar--*.sql 2>/dev/null | grep -v archive | head -n 1); \
	if [ -z "$$CURRENT_SQL" ]; then \
		echo "Error: No SQL file found in $$PGSTAR_DIR/"; \
		exit 1; \
	fi; \
	CURRENT_VERSION=$$(basename "$$CURRENT_SQL" | sed "s/pgstar--\(.*\)\.sql/\1/"); \
	\
	# Determine new version \
	if [ -n "$${PGSTAR_VERSION:-}" ]; then \
		# Explicit version specified \
		NEW_VERSION="$$PGSTAR_VERSION"; \
	elif [ -f make_bump.version ]; then \
		# Read from version file \
		VERSION_FILE=$$(cat make_bump.version | tr -d "[:space:]"); \
		if [ "$$VERSION_FILE" = "$$CURRENT_VERSION" ]; then \
			# Same version - auto-increment patch version \
			MAJOR_MINOR=$$(echo "$$CURRENT_VERSION" | sed "s/\.[0-9]*$$//"); \
			PATCH=$$(echo "$$CURRENT_VERSION" | sed "s/.*\.//"); \
			NEW_PATCH=$$((PATCH + 1)); \
			NEW_VERSION="$$MAJOR_MINOR.$$NEW_PATCH"; \
			echo "  Auto-incrementing patch version: $$CURRENT_VERSION -> $$NEW_VERSION"; \
		else \
			# Different version specified in file \
			NEW_VERSION="$$VERSION_FILE"; \
		fi; \
	else \
		echo "Error: PGSTAR_VERSION not set and make_bump.version file not found"; \
		exit 1; \
	fi; \
	if [ -z "$$NEW_VERSION" ]; then \
		echo "Error: Version is empty"; \
		exit 1; \
	fi; \
	echo "Bumping pgstar extension to version $$NEW_VERSION..."; \
	echo "  Current version: $$CURRENT_VERSION"; \
	echo "  New version: $$NEW_VERSION"; \
	\
	# Update control file \
	if [ -f "$$CONTROL_FILE" ]; then \
		sed -i "s/default_version = .*/default_version = '\''$$NEW_VERSION'\''/" "$$CONTROL_FILE"; \
		echo "  Updated $$CONTROL_FILE"; \
	else \
		echo "Warning: $$CONTROL_FILE not found"; \
	fi; \
	\
	# Update version header file \
	VERSION_HEADER="$$PGSTAR_DIR/generated/pgstar_version.h"; \
	mkdir -p "$$PGSTAR_DIR/generated"; \
	echo "#ifndef PGSTAR_VERSION_STRING" > "$$VERSION_HEADER"; \
	echo "#define PGSTAR_VERSION_STRING \"$$NEW_VERSION\"" >> "$$VERSION_HEADER"; \
	echo "#endif" >> "$$VERSION_HEADER"; \
	echo "  Updated $$VERSION_HEADER"; \
	\
	# Copy SQL file to new version (only if different) \
	NEW_SQL="$$PGSTAR_DIR/pgstar--$$NEW_VERSION.sql"; \
	if [ "$$CURRENT_SQL" != "$$NEW_SQL" ]; then \
		cp "$$CURRENT_SQL" "$$NEW_SQL"; \
		echo "  Created $$NEW_SQL"; \
		\
		# Archive old SQL file \
		ARCHIVE_DIR="$$PGSTAR_DIR/generated/archive"; \
		mkdir -p "$$ARCHIVE_DIR"; \
		OLD_SQL_NAME=$$(basename "$$CURRENT_SQL"); \
		mv -f "$$CURRENT_SQL" "$$ARCHIVE_DIR/$$OLD_SQL_NAME"; \
		echo "  Archived $$CURRENT_SQL -> $$ARCHIVE_DIR/$$OLD_SQL_NAME"; \
	else \
		echo "  SQL file already exists for version $$NEW_VERSION"; \
	fi; \
	\
	# Update Makefile DATA line \
	if [ -f "$$MAKEFILE" ]; then \
		sed -i "s|^DATA = .*|DATA = pgstar--$$NEW_VERSION.sql|" "$$MAKEFILE"; \
		echo "  Updated $$MAKEFILE DATA line"; \
	else \
		echo "Warning: $$MAKEFILE not found"; \
	fi; \
	\
	# Update version file \
	echo "$$NEW_VERSION" > make_bump.version; \
	echo "  Updated make_bump.version"; \
	echo ""; \
	echo "Version bump complete! New version: $$NEW_VERSION"; \
	echo "Rebuilding extension with new version..."; \
	cd "$$PGSTAR_DIR" && make clean && make && make install'
	# @make auto
