#!/usr/bin/env bash
# The duckdb submodule must sit on the tag we ship against. A drifted submodule
# makes every local green meaningless. This is a stub until the submodule is
# checked out and a target tag is chosen (see docs/hld.md §7.1).
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f duckdb/CMakeLists.txt ]; then
	echo "check_duckdb_pin: duckdb submodule not checked out yet — nothing to verify."
	echo "  run: git submodule update --init --recursive"
	exit 0
fi

ver=$(git -C duckdb describe --tags 2>/dev/null || echo "unknown")
echo "check_duckdb_pin: duckdb submodule at ${ver}"
# TODO: once a target tag is chosen, assert ver matches it and fail otherwise.
