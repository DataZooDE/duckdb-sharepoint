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

## Current status

Scaffold. Pure layer implemented + tested; Graph layer written against the docs
but uncompiled and unrun. See `docs/hld.md` §7 for the remaining work in order.
The first thing any contributor must do is check out the `duckdb` submodule at
the pinned tag and get it to compile — expect to reconcile exact DuckDB API
signatures (`FileSystem`, `SecretManager`, `FileOpener`).
