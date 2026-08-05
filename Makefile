PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sharepoint
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# extension-ci-tools defines `test: test_release`, which runs only the SQL
# suite. That would make `make test` green while the Catch2 pure-logic binary
# was never executed. Override so the headline target runs BOTH layers.
# ---------------------------------------------------------------------------
.PHONY: test
test: unit_test test_release

# ---------------------------------------------------------------------------
# Catch2 unit tests -- pure logic, no DuckDB linkage, no network, no
# credentials. Always runnable by anyone who can build.
# ---------------------------------------------------------------------------
.PHONY: unit_test
unit_test: release
	@if [ ! -x build/release/test/sharepoint_unit_tests ]; then \
		echo "build/release/test/sharepoint_unit_tests not found -- check Catch2 in vcpkg.json"; \
		exit 1; \
	fi
	./build/release/test/sharepoint_unit_tests --reporter console::out=-

# ---------------------------------------------------------------------------
# Live SQL tests against a REAL Microsoft Graph / SharePoint tenant. Without
# credentials these skip cleanly via `require-env`, so `make test` stays green
# for a developer who has none. test_live FAILS LOUDLY when they are absent.
# ---------------------------------------------------------------------------
.PHONY: test_live
test_live: release
	@./scripts/run_live_tests.sh

# ---------------------------------------------------------------------------
# The duckdb submodule must sit on the tag we ship against.
# ---------------------------------------------------------------------------
.PHONY: check_pin
check_pin:
	@./scripts/check_duckdb_pin.sh

# ---------------------------------------------------------------------------
# Credential hygiene. Fails if anything matching a secret pattern is tracked.
# ---------------------------------------------------------------------------
.PHONY: check_credentials
check_credentials:
	@./scripts/check_no_credentials.sh
