# How we work on duckdb-sharepoint

This is a DuckDB C++ extension registering a `sharepoint://` filesystem over
Microsoft Graph. It is the sibling of `duckdb-gdrive`; when a question is not
answered here, that repo's `CLAUDE.md` and code are the reference.

## Tests — two layers, no mocks anywhere

1. **Pure-logic layer (Catch2).** Sources with no `#include "duckdb.hpp"` and no
   I/O — URI parsing, glob matching, error classification, version. They are
   compiled into both the extension and `sharepoint_unit_tests`, and because
   they do no I/O there is nothing to mock. `make unit_test`.
2. **Live SQL layer.** Everything Graph-facing is proven only by SQL tests
   against a **real** Microsoft Graph tenant — no fake, no replay. Without
   credentials they skip cleanly (`require-env`), so `make test` stays green
   locally; `make test_live` fails loudly when credentials are absent (CI).

`make test` runs BOTH layers. A task is done when the live SQL test for it
passes against a real tenant.

## The C++17 / picojson / httplib gotchas

Inherited wholesale from gdrive — see `extension_config.cmake` and
`CMakeLists.txt` comments:
- Force `CMAKE_CXX_STANDARD 17` for the **entire** build (weak/strong symbol
  split on Linux; MSVC `fmt` inline variables otherwise).
- Define `PICOJSON_USE_INT64` project-wide (else parsed numbers are doubles —
  an ODR trap, not a preference).
- Use DuckDB's bundled `duckdb_httplib_openssl`, not a separate vcpkg dep.

## Secrets

Never commit a credential. `.gitignore` + `scripts/check_no_credentials.sh`
enforce it. The OAuth scope option is `GRAPH_SCOPE`, never `SCOPE` (DuckDB
reserves `SCOPE` for the secret's path-prefix list).

## Working contract

- Red → green: write the failing test first (the pure layer where possible, the
  live SQL layer for anything Graph-facing).
- One logical change per PR; keep the DuckDB submodule pinned (`make check_pin`).
- When a non-obvious Graph behaviour bites (throttling shapes, upload-session
  chunk rules, cTag vs eTag), write it down in `docs/hld.md` or a discovered
  note so it is not rediscovered.

## Pitfall: a failed COPY "cleans up" its target (2026-11-13)

On DuckDB v1.5.6 a COPY that fails after its writer initialised calls
`TryRemoveFile` on every path in `created_files`. The destructor of
`CopyToFunctionGlobalState` does this, and the COPY's target is always in
that list. For the in-place writers (excel_range / excel_table, which are
`USE_TMP_FILE false`) the target is the user's workbook. Measured before the
fix, on a sharepoint:// path, a browser URL or a sharing link: the DELETE went
out (`delete = 1` in `sharepoint_stats()`). Only Graph's `423 resourceLocked`
stopped it, because the failed COPY left its workbook session open. That held
even after 420 s of idle, but it is luck and not design. With a BARE target
('Finance/plan.xlsx') the cleanup went to the LOCAL filesystem. A same-named
file in the working directory was deleted. Fixed: the writer registers its
target (`InPlaceCopyTarget`), `SharepointFileSystem::TryRemoveFile` skips a
registered path, and the Excel rewrite adds `sharepoint://` to a bare target.
`e2e/tests/test_failed_copy.py` asserts `delete = 0` and not just "the
workbook survived", because the 423 makes the second check pass without the
fix. Not covered: `sharepoint_list` and OneNote targets ('Orders',
'Notebook/Section') are not sharepoint paths, so their cleanup still goes to
the local filesystem. Found first in duckdb-gdrive (FORMAT sheet_range,
DataZooDE/duckdb-gdrive#56).

A failed Excel COPY also leaves its workbook session open. The workbook stays
locked (423 on delete) for minutes, so the e2e `scratch` teardown can warn
and leave its folder for `make sweep_orphans`.

## Current status

Verified end to end against a real tenant (2026-10-10): `make test`,
`make test_live` and `make e2e` (out-of-band changes, restarts, 16-thread
scans, DuckLake, ATTACH, two drives) are green on DuckDB v1.5.6; CI also
builds the DuckDB 2.0 line (1.4.x LTS is no longer supported). Structurally aligned with duckdb-gdrive (CI/pin gates,
stats, banner/telemetry, glob syntax, shared VFS functions, e2e harness).
Writes stream in bounded memory (one chunk, `deferCommit` upload session;
2026-10-14, hld §7 item 6a). `docs/hld.md` §7/§7a has the remaining work and the Graph behaviours found
live; `docs/exploratory-testing.md` the exploratory findings. Repo secrets
`SHAREPOINT_*` must be set for the CI live job. First release: v2026.10.16;
the release and community-extensions procedure is `docs/RELEASE.md`.
