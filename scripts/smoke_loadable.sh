#!/usr/bin/env bash
# Load the SHIPPED artifact into a STOCK DuckDB and use it.
#
# Everything else in this repo tests `build/release/duckdb` -- a shell with
# the extension statically linked. That is not what users install. Symbol
# resolution and the DuckDB-version handshake only happen at LOAD time, so a
# statically linked build can be green while the loadable artifact is
# unusable (the sibling duckdb-gdrive shipped exactly that once: a stale CMake
# cache stamped the artifact for a commit hash, every test stayed green).
#
# Skips (rather than fails) when no matching stock duckdb is on PATH -- the
# version must match exactly. Under CI a skip is a FAILURE: "installed the CLI"
# and "the check ran" are different facts. SMOKE_LOADABLE_STRICT=0 opts out,
# =1 demands the real check locally.
set -euo pipefail

cd "$(dirname "$0")/.."

EXT="$PWD/build/release/extension/sharepoint/sharepoint.duckdb_extension"
# Keep in step with scripts/check_duckdb_pin.sh and the stable workflow job.
# shellcheck source=duckdb_version.env
source scripts/duckdb_version.env
WANT_VERSION="$DUCKDB_TAG"
STRICT="${SMOKE_LOADABLE_STRICT:-${CI:+1}}"
STRICT="${STRICT:-0}"

skip_or_fail() {
    if [[ "$STRICT" == "1" ]]; then
        echo "FAIL: $1" >&2
        echo "      Refusing to report success for a check that did not run." >&2
        exit 1
    fi
    echo "SKIP: $1"
    exit 0
}

if [[ ! -f "$EXT" ]]; then
    echo "FAIL: $EXT not built. Run \`make\` first." >&2
    exit 1
fi

if ! command -v duckdb >/dev/null 2>&1; then
    skip_or_fail "no stock \`duckdb\` on PATH to load the artifact into."
fi

have="$(duckdb -noheader -list -c "SELECT version();" 2>/dev/null || true)"
if [[ "$have" != "$WANT_VERSION" ]]; then
    skip_or_fail "stock duckdb is ${have:-unknown}, artifact targets $WANT_VERSION."
fi

echo "==> loading the shipped artifact into stock duckdb $have"

# Loading proves the handshake; calling a function proves the symbols resolved.
# `|| true`: under `set -e` a failing duckdb would otherwise exit here with no
# message; the grep below turns a failure into a loud FAIL with the output.
out="$(duckdb -unsigned -noheader -list -c "
LOAD '$EXT';
SELECT 'version=' || sharepoint_version();
" 2>&1 || true)"

if ! grep -q '^version=' <<<"$out"; then
    echo "FAIL: the shipped artifact did not load." >&2
    echo "$out" >&2
    exit 1
fi
echo "$out" | sed 's/^/    /'

# With credentials, do the real thing: authenticate and list the drive root
# through the shipped artifact. The secret values travel via environment
# variables DuckDB reads with getenv(), never on a command line.
if [[ -n "${SHAREPOINT_TENANT_ID:-}" && -n "${SHAREPOINT_CLIENT_ID:-}" \
      && -n "${SHAREPOINT_CLIENT_SECRET:-}" && -n "${SHAREPOINT_DRIVE_ID:-}" ]]; then
    echo "==> listing the real drive through the shipped artifact"
    live="$(duckdb -unsigned -noheader -list -c "
LOAD '$EXT';
CREATE SECRET smoke (TYPE sharepoint, PROVIDER client_credentials,
    TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CLIENT_ID'),
    CLIENT_SECRET getenv('SHAREPOINT_CLIENT_SECRET'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));
SELECT 'listed=' || (count(*) >= 0) FROM glob('sharepoint://*');
" 2>&1 || true)"
    if ! grep -q '^listed=true$' <<<"$live"; then
        echo "FAIL: the shipped artifact loaded but could not reach Graph." >&2
        echo "$live" >&2
        exit 1
    fi
    echo "OK: shipped artifact authenticates and lists a REAL drive from stock duckdb $have"
else
    echo "OK: shipped artifact loads and resolves in a stock duckdb $have"
    echo "    (no credentials set -- did NOT call Graph through it)"
fi
