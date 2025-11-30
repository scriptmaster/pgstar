.PHONY: extension-install extension-clean restart test run auto auto-run log logs tail psql e2e print-pwd fetch-assets

# Default target
all: extension-install auto

install: extension-install

# Extension management
extension-install:
	@make -s -C hello_world
	@make -s -C hello_world install
	@make -s -C pgstar
	@make -s -C pgstar install

extension-clean:
	@make -s -C pgstar clean
	@make -s -C hello_world clean

# Server management
restart:
	@sudo systemctl restart postgresql

# Testing and Running
test: extension-install restart
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -c "SELECT 1" > /dev/null

run: extension-install restart
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost -f ./run.sql

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
