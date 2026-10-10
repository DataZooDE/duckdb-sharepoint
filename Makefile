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

# ---------------------------------------------------------------------------
# musl (a 1.4 LTS distribution target) does not pull <cstdint> in
# transitively the way glibc does; a missing include breaks only that target.
# ---------------------------------------------------------------------------
.PHONY: check_cstdint
check_cstdint:
	@./scripts/check_cstdint.sh

# ---------------------------------------------------------------------------
# The SHIPPED artifact, not the statically linked shell: it must be stamped
# for the pinned DuckDB, link only platform libs, and load into a stock CLI.
# ---------------------------------------------------------------------------
.PHONY: check_stamp
check_stamp: release
	@./scripts/check_extension_stamp.sh

.PHONY: smoke_static
smoke_static: release
	@./scripts/check_static_linkage.sh \
		build/release/extension/sharepoint/sharepoint.duckdb_extension

.PHONY: smoke_loadable
smoke_loadable: release
	@./scripts/smoke_loadable.sh

# ---------------------------------------------------------------------------
# Live end-to-end harness (Python + uv), ported from duckdb-gdrive: what SQL
# tests cannot express -- a change made OUTSIDE a running DuckDB process, a
# restart, a multi-threaded scan. Needs SHAREPOINT_* credentials (environment
# or a git-ignored .env.sharepoint). Every test works in its own scratch
# folder under /_duckdb_sharepoint_e2e and deletes it on teardown.
# ---------------------------------------------------------------------------
.PHONY: e2e
e2e: release
	@cd e2e && uv run pytest -v

# Delete scratch folders crashed runs leaked (older than 24 h).
.PHONY: sweep_orphans
sweep_orphans:
	@cd e2e && uv run python -m helpers.sweep
