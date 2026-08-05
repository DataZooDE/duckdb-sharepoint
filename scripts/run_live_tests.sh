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

TEMPLATE=test/sql/sharepoint.live.template
LIVE_DIR=test/sql/live
LIVE="$LIVE_DIR/sharepoint.live.test"
UNITTEST=build/release/test/unittest

[ -f "$TEMPLATE" ] || { echo "ERROR: $TEMPLATE missing."; exit 1; }
if [ ! -x "$UNITTEST" ]; then
	echo "ERROR: $UNITTEST not found — run 'make release' first."
	exit 1
fi

mkdir -p "$LIVE_DIR"

# The materialised copy carries the run's REAL client secret, so it is written
# only into the git-ignored test/sql/live/ and scrubbed on exit (belt to
# .gitignore's braces). Substitution is a plain str.replace in Python, not sed:
# a client secret can contain /, & and other sed-active characters.
cleanup() { rm -f "$LIVE"; }
trap cleanup EXIT

python3 - "$TEMPLATE" "$LIVE" <<'PY'
import os, sys

src, dst = sys.argv[1], sys.argv[2]
placeholders = {
    "@SHAREPOINT_TENANT_ID@": os.environ["SHAREPOINT_TENANT_ID"],
    "@SHAREPOINT_CLIENT_ID@": os.environ["SHAREPOINT_CLIENT_ID"],
    "@SHAREPOINT_CLIENT_SECRET@": os.environ["SHAREPOINT_CLIENT_SECRET"],
    "@SHAREPOINT_DRIVE_ID@": os.environ["SHAREPOINT_DRIVE_ID"],
}
text = open(src, "r", encoding="utf-8").read()
for key, value in placeholders.items():
    if "'" in value:
        sys.exit(f"ERROR: {key[1:-1]} contains a single quote, which would break the SQL string literal.")
    text = text.replace(key, value)
open(dst, "w", encoding="utf-8").write(text)
PY

echo "run_live_tests: materialised $LIVE — running against the live tenant."
"$UNITTEST" "$LIVE"
