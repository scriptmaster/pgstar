.PHONY: extension-install extension-clean restart test run auto auto-run log logs tail psql

# Default target
all: extension-install

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
log logs:
	@echo "Tailing latest PostgreSQL log file under /var/lib/postgresql/14/main/log (Ctrl+C to stop)..."
	@sudo sh -c 'latest=$$(ls -1t /var/lib/postgresql/14/main/log/*.log 2>/dev/null | head -n 1); \
		if [ -z "$$latest" ]; then \
			echo "No PostgreSQL log files found in /var/lib/postgresql/14/main/log"; \
			exit 1; \
		fi; \
		echo "Following $$latest"; \
		tail -f "$$latest"'

tail:
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
		make log || true; \
		echo "Log tail ended, restarting auto-run..."; \
		sleep 1; \
	done

# Direct PSQL access
psql:
	@PGPASSWORD="pgstar" psql -U pgstar -d pgstar -h localhost
