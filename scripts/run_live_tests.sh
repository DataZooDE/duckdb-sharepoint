#!/usr/bin/env bash
# Live SQL tests against a REAL Microsoft Graph tenant. Fails loudly when
# credentials are absent -- use in CI where a silent skip would be a false
# green. (`make test` skips these cleanly instead.)
#
# Required env:
#   SHAREPOINT_TENANT_ID   Entra ID tenant (GUID or domain)
#   SHAREPOINT_CLIENT_ID   app registration (client) id
#   SHAREPOINT_CLIENT_SECRET
#   SHAREPOINT_DRIVE_ID    a Graph drive id (a document library)
set -euo pipefail
cd "$(dirname "$0")/.."

missing=0
for v in SHAREPOINT_TENANT_ID SHAREPOINT_CLIENT_ID SHAREPOINT_CLIENT_SECRET SHAREPOINT_DRIVE_ID; do
	if [ -z "${!v:-}" ]; then
		echo "ERROR: $v is not set — live tests cannot run."
		missing=1
	fi
done
[ "$missing" -eq 0 ] || exit 1

BIN=build/release/duckdb
if [ ! -x "$BIN" ]; then
	echo "ERROR: $BIN not found — run 'make release' first."
	exit 1
fi

# Materialise the live template with the run's real ids into test/sql/live/
# (git-ignored), then run it. The template lives at test/sql/sharepoint.live.template
# once written (docs/hld.md §7.5); until then this is the harness skeleton.
echo "run_live_tests: credentials present — TODO: materialise + run test/sql/live/ (see docs/hld.md §7.5)"
