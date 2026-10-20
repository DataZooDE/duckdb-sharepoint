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
#
# Delegated (user) auth, run in addition when BOTH are set (written by
# scripts/delegated_login.py into the git-ignored .env.sharepoint.delegated):
#   SHAREPOINT_DELEGATED_CLIENT_ID  the public-client app registration
#   SHAREPOINT_USER_REFRESH_TOKEN   a delegated refresh token for a test user
# Set SHAREPOINT_REQUIRE_DELEGATED=1 to fail instead of skipping without them.
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

LIVE_DIR=test/sql/live
UNITTEST=build/release/test/unittest

if [ ! -x "$UNITTEST" ]; then
	echo "ERROR: $UNITTEST not found — run 'make release' first."
	exit 1
fi

delegated=0
if [ -n "${SHAREPOINT_DELEGATED_CLIENT_ID:-}" ] && [ -n "${SHAREPOINT_USER_REFRESH_TOKEN:-}" ]; then
	delegated=1
elif [ "${SHAREPOINT_REQUIRE_DELEGATED:-0}" = "1" ]; then
	echo "ERROR: SHAREPOINT_DELEGATED_CLIENT_ID / SHAREPOINT_USER_REFRESH_TOKEN not set — run scripts/delegated_login.py."
	exit 1
else
	echo "run_live_tests: SKIP delegated suite (SHAREPOINT_DELEGATED_CLIENT_ID / SHAREPOINT_USER_REFRESH_TOKEN not set)."
fi

mkdir -p "$LIVE_DIR"

# The materialised copies carry the run's REAL credentials, so they are written
# (mode 0600) only into the git-ignored test/sql/live/ and scrubbed on exit
# (belt to .gitignore's braces). Substitution is a plain str.replace in Python,
# not sed: a client secret can contain /, & and other sed-active characters.
cleanup() { rm -f "$LIVE_DIR"/*.live.test; }
trap cleanup EXIT

materialise() {
	python3 - "$1" "$2" <<'PY'
import os, sys

src, dst = sys.argv[1], sys.argv[2]
names = ["SHAREPOINT_TENANT_ID", "SHAREPOINT_CLIENT_ID", "SHAREPOINT_CLIENT_SECRET", "SHAREPOINT_DRIVE_ID",
         "SHAREPOINT_DELEGATED_CLIENT_ID", "SHAREPOINT_USER_REFRESH_TOKEN"]
text = open(src, "r", encoding="utf-8").read()
# A per-run id for paths that must not collide with an earlier run's leftovers
# (not a secret; only [0-9A-Za-z-], safe in a Graph item name and a SQL string).
import datetime
run_id = os.environ.get("SHAREPOINT_LIVE_RUN_ID") or (
    datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + os.urandom(4).hex())
text = text.replace("@RUN_ID@", run_id)
for name in names:
    key = f"@{name}@"
    if key not in text:
        continue
    value = os.environ[name]
    if "'" in value:
        sys.exit(f"ERROR: {name} contains a single quote, which would break the SQL string literal.")
    text = text.replace(key, value)
fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "w", encoding="utf-8") as f:
    f.write(text)
PY
}

suites=(sharepoint)
if [ "$delegated" -eq 1 ]; then
	suites+=(sharepoint_delegated)
fi
for suite in "${suites[@]}"; do
	template="test/sql/$suite.live.template"
	live="$LIVE_DIR/$suite.live.test"
	[ -f "$template" ] || { echo "ERROR: $template missing."; exit 1; }
	materialise "$template" "$live"
	echo "run_live_tests: materialised $live — running against the live tenant."
	"$UNITTEST" "$live"
done
