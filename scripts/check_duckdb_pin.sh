#!/usr/bin/env bash
# The duckdb submodule must sit on the tag we ship against. A drifted submodule
# makes every local green meaningless. We pin to the 1.4 LTS line: the
# extension_config.cmake special-casing (_HAS_STD_BYTE=0, the sqlite3 wrapper
# note) is written for it, and it is the current DuckDB long-term-support
# release (see docs/hld.md §7.1).
set -euo pipefail
cd "$(dirname "$0")/.."

# The exact tag this repo builds and ships against. Bump deliberately, together
# with the matching extension-ci-tools branch (v1.4-andium) and a rebuild.
PINNED_DUCKDB_TAG="v1.4.5"

if [ ! -f duckdb/CMakeLists.txt ]; then
	echo "check_duckdb_pin: duckdb submodule not checked out yet — nothing to verify."
	echo "  run: git submodule update --init --recursive"
	exit 0
fi

ver=$(git -C duckdb describe --tags 2>/dev/null || echo "unknown")
if [ "${ver}" != "${PINNED_DUCKDB_TAG}" ]; then
	echo "check_duckdb_pin: FAIL — duckdb submodule is at '${ver}', expected '${PINNED_DUCKDB_TAG}'."
	echo "  A drifted submodule makes every local green meaningless. To move the pin,"
	echo "  edit PINNED_DUCKDB_TAG in this script and check the submodule out to match."
	exit 1
fi
echo "check_duckdb_pin: OK — duckdb submodule at ${ver} (pinned ${PINNED_DUCKDB_TAG})."
