# duckdb-sharepoint — high-level design

## 1. Purpose

Register a `sharepoint://` virtual filesystem in DuckDB backed by **Microsoft
Graph**, so every path-taking DuckDB surface (`read_parquet`, `read_csv`,
`COPY`, `glob`, DuckLake `DATA_PATH`) can address a SharePoint / OneDrive
document library directly. The design mirrors the sibling
[`duckdb-gdrive`](https://github.com/DataZooDE/duckdb-gdrive); this document
records only where SharePoint differs and what remains to implement.

## 2. Why this extension exists

escurel's `LaneStore` has a DuckDB-VFS backend (`duckvfs`) that speaks no
provider API — it issues scheme-generic SQL (`read_blob`, `glob`, `write_blob`,
`remove_file`, `move_file`, `file_size`) and lets DuckDB dispatch on the URL
scheme. It already runs on `gdrive://`. Pointing it at `sharepoint://` needs a
DuckDB `FileSystem` registered for that scheme — this extension — plus the same
four mutation SQL functions, which ship here verbatim from gdrive.

## 3. What Graph makes simpler than Drive

| concern | Google Drive (gdrive) | Microsoft Graph (here) |
|---|---|---|
| path → object | **no path addressing**; one `files.list` per segment; needs a path→id cache (the project's biggest risk, `R-1`) | **native**: `/drives/{id}/root:/{path}` resolves in one call — **no cache needed** |
| duplicate names | allowed; needs `R-4` disambiguation to an `id:` form | **forbidden** by SharePoint; `R-4` does not arise |
| content addressing | `files/{id}?alt=media` | `/items/{id}/content` (302 → pre-authed URL), honours `Range` |
| version tag | `headRevisionId` | `cTag` (content tag; `eTag` also moves on metadata edits) |
| write | buffered whole-file upload on close | same — plus a resumable **upload session** for large files |

The net effect: this filesystem is materially smaller than gdrive's. There is
no `sharepoint_path_cache.*`, no `DisambiguatePath`, and `Glob`/`ListFiles` map
straight onto `/children`.

## 4. Addressing

```
sharepoint://Finance/2026/actuals.parquet   -> /drives/{drive}/root:/Finance/2026/actuals.parquet
sharepoint://id:01ABCDEF                     -> /drives/{drive}/items/01ABCDEF
```

Which drive (and site) is a property of the **secret** (`DRIVE_ID`, or
`SITE_ID` → the site's default document library via `/sites/{id}/drive`), never
of the URI — so a corpus is portable across tenants by swapping the secret.
Parsing lives in the pure `sharepoint_uri.*` and is unit-tested.

## 5. Authentication (`sharepoint_auth.*`)

App-only **client credentials** is the default and the escurel path:

```
POST {login}/{tenant}/oauth2/v2.0/token
  grant_type=client_credentials & client_id & client_secret
  & scope=https://graph.microsoft.com/.default
```

Tokens are cached in-process keyed by a **SHA-256 fingerprint of the
credential material** (provider, tenant, client id/secret, refresh token,
scope, login endpoint) — never by the secret's name, which a recreated or
per-connection secret can reuse with different credentials. The block cache
and the move same-drive check key on a fingerprint of credentials + Graph cloud
+ drive. File handles hold a `TokenSource`, not a token, so a long scan or an
ATTACHed database re-mints before expiry (and once on an unexpected 401). The
Graph and login endpoints are session settings, so only Microsoft cloud hosts
over https are accepted. A `config` secret with a bare `ACCESS_TOKEN` is used
as-is; with a `REFRESH_TOKEN` it uses the `refresh_token` grant.

## 6. Read / write / mutate (`sharepoint_client.*`, `sharepoint_filesystem.*`)

- **Read** — ranged `GET /items/{id}/content`. A shared block cache (as in
  gdrive, amortising per-request latency over larger blocks) is *declared*
  (`sharepoint_block_size_bytes`) but not yet implemented; reads are exact
  ranges for now.
- **Write** — bytes buffer in the handle and upload on `Close()`. Positional
  writes and `Truncate`/`Trim` throw (Graph replaces whole files). A single
  `PUT .../content` for files ≤ `sharepoint_upload_chunk_bytes`.
- **Delete** — `DELETE /items/{id}` (recycle bin; `Prefer: permanent-delete`
  behind `sharepoint_permanent_delete`).
- **Move** — `PATCH /items/{id}` with `parentReference` + `name`. Not atomic —
  documented, as in gdrive.
- **Glob / list** — `GET .../children` with `@odata.nextLink` paging.

## 7. Remaining work (the honest TODO list)

1. ~~**Compile against a pinned DuckDB submodule.**~~ **Done (2026-08-05).**
   Pinned **duckdb `v1.4.5`** (latest 1.4 LTS — the line `extension_config.cmake`
   is written for) and **extension-ci-tools `v1.4-andium`**; vcpkg at the
   `MainDistributionPipeline.yml` commit. The scaffold compiles clean and both
   test layers pass (pure Catch2 green; live SQL skips via `require-env`).
   `scripts/check_duckdb_pin.sh` now asserts the tag. The `FileSystem` /
   `SecretManager` / `FileOpener` signatures all matched the scaffold as
   written; only two build wiring fixes were needed, both worth remembering:
   - **picojson** — the vcpkg port (≥1.3.0) installs the header at
     `include/picojson/picojson.h`, but the sources spell it `<picojson.h>`
     (as gdrive does). `CMakeLists.txt` now `find_path`s the subdir and adds it
     to the include path, rather than rewriting every `#include`.
   - **`FileOpener`** — `file_system.hpp` only forward-declares it, so calling
     `FileOpener::TryGetClientContext` needs an explicit
     `#include "duckdb/common/file_opener.hpp"` (added to `sharepoint_filesystem.cpp`).

   The submodule **gitlinks** are recorded in the pin commit (they were
   direct-cloned into place first), and `MainDistributionPipeline.yml` is
   aligned to the same pins (`duckdb_version: v1.4.5`, the reusable workflow ref
   and `ci_tools_version` both the v1.4-andium commit `1f04702`).
2. ~~**Resumable upload session** (`PutContent`, large-file branch).~~
   **Implemented (2026-08-05); verified live 2026-10-10.** After
   `createUploadSession`, the `uploadUrl` is split into origin + path
   (`SplitAbsoluteUrl`) and driven by its own httplib client with **no** auth
   header. Chunks are `NormalizeUploadChunkSize`d down to a 320-KiB multiple,
   each PUT carries a `Content-Range` (`UploadContentRange`); 202 advances,
   200/201 on the final chunk returns the driveItem, and a transport/5xx/429
   failure resumes from the session's `nextExpectedRanges` (`ParseRangeStart`,
   `QueryUploadResumeOffset`) for up to `kMaxUploadChunkRetries` attempts. The
   four pure helpers live in `sharepoint_upload.*` and are Catch2-tested; the
   PUT loop is in `sharepoint_client.cpp`. **Live proof still owed:** the §7.5
   harness must upload a file **larger than one chunk** to exercise this branch
   end to end — nothing here has run against a tenant.
3. ~~**Block cache** for reads (`sharepoint_block_size_bytes`), single-flight per
   block, identity-keyed.~~ **Implemented (2026-08-05); verified live 2026-10-10.**
   `SharepointBlockCache` (`sharepoint_blockcache.*`, pure — the byte fetch is
   injected) serves reads from fixed-size blocks: a ranged GET pays a round trip
   regardless of size, so a Parquet scan's many small adjacent reads collapse
   onto a few cached blocks. Single-flight per block (a per-block load mutex, so
   concurrent readers of one block share one fetch), byte-bounded LRU eviction,
   and process-wide (the FileSystem is one object for the DatabaseInstance). Keys
   are `identity | item-id | cTag`, so it is never a cross-tenant or stale-version
   hit. `FileSystem::Read` routes through it, supplying a fetch backed by
   `GraphClient::ReadRange`; `sharepoint_block_size_bytes = 0`, or an
   out-of-bounds read, falls back to the exact-range path. The block math and the
   cache (fetch-count, hit, and LRU-eviction behaviour) are Catch2-tested with a
   synthetic fetcher; the concurrency is single-flight by construction but only
   single-thread-tested here, and no cache has run against a live tenant.
4. ~~**Recursive `**` glob** across folders (currently one folder level).~~
   **Implemented (2026-08-05); verified live 2026-10-10.** `Glob()` now walks
   the folder tree: `*`/`?` match within a segment, `**` matches zero or more
   whole levels, and only files are emitted (a trailing `**` yields every file
   beneath the folder). A fully literal path keeps its one-`Stat` fast path. The
   walk itself is a pure matcher (`GlobFiles` in `sharepoint_glob.*`, driven by
   an injected lister and unit-tested over a synthetic tree, each folder listed
   at most once); `FileSystem::Glob` only adapts the lister to
   `GraphClient::ListChildren`. Live proof (a real nested library) is owed to
   the §7.5 harness. Note: `GlobFiles` is deliberately called qualified —
   `FileSystem` has a same-named member that would otherwise shadow it.
5. **Live SQL suite** (`test/sql/live/`, `make test_live`) against a real
   tenant. **SQL layer scaffolded (2026-08-05); a Python e2e harness (fixtures,
   API-call-count assertions) is still owed.** `test/sql/sharepoint.live.template`
   (committed, placeholders only) exercises the full surface end to end: the
   write/read/size round trip, idempotent `remove_file`, `COPY`→`read_parquet`,
   the **>1-chunk resumable upload** (§7.2, forced by `SET
   sharepoint_upload_chunk_bytes=327680` + a 900 KB blob), the **nested `**`
   glob** (§7.4), and `move_file`. `scripts/run_live_tests.sh` substitutes the
   run's real ids into the git-ignored `test/sql/live/` (scrubbed on exit) and
   runs it through `build/release/test/unittest`. Verified end to end with dummy
   credentials: it materialises, loads, parses, and **fails loudly** at the first
   Graph call — a real tenant is all that is missing.
   - **Gotcha worth keeping:** DuckDB's sqllogictest runner defaults to
     `ignore_error_messages = {"HTTP", "Unable to connect"}` and *skips* any test
     whose error contains them. Our Graph errors read `HTTP <status>`, so without
     intervention every real failure would be a false green. The template resets
     the list to a never-matching sentinel (`set ignore_error_messages
     __sharepoint_never_ignore__`) so failures fail.
6. **`authorization_code` / `device_code`** delegated providers via the
   `datazoo-oauth2` submodule. *Still open.*
6b. **DNS is not bounded by the pre-auth transport's deadline.** Connect,
   handshake, send and receive are; `getaddrinfo` inside `BIO_do_connect`
   blocks for the OS resolver timeout. Bounding it needs a resolver thread.
   *Open (crew round 4, F8; low).*
6a. **Streaming writes.** A write is buffered in memory and uploaded on
   close (gdrive does the same); a multi-GB COPY needs that much RAM. Stream
   full chunks into the upload session instead. *Open (crew round 3, F6).*
7. **CI**: ~~wire `MainDistributionPipeline.yml` to the DuckDB extension-ci-tools
   reusable workflow~~ **done**, plus **erpl.io deployment wired (2026-08-06)** —
   `_extension_deploy.yml` + `scripts/extension-upload.sh` ported from the
   sibling anofox-statistics, with a `duckdb-lts-deploy` job (gated to
   `main`/`v*` tags so PRs never need OIDC) publishing to the erpl.io bucket, so
   users can `INSTALL sharepoint FROM 'http://get.erpl.io'` (DuckDB `-unsigned`).
   Two org-side prerequisites must be granted for the deploy to succeed: the
   repo variable `DEPLOY_S3_BUCKET`, and adding `DataZooDE/duckdb-sharepoint` to
   the trust policy of the `ErplGithubOicdRole` IAM role. Still open: the
   community-extensions submission, and a signing key (`DUCKDB_EXTENSION_SIGNING_PK`)
   if a signed build is ever wanted.

## 7a. Discovered on the first live runs (2026-10-10)

Things the Graph docs do not tell you, each found against a real tenant and
each guarded by a test so it is not rediscovered:

- **Signed URLs must be sent byte for byte.** `GET .../items/{id}/content`
  302s to `https://<tenant>.sharepoint.com/.../download.aspx?UniqueId=..&
  Translate=false&tempauth=..&ApiVersion=2.0`; the upload session's
  `uploadUrl` has the same shape. SharePoint validates the query string AS
  SENT: re-encoding it is fine, but **any reordering is a 401**. cpp-httplib
  0.27 (bundled with DuckDB 1.5) parses every request query into a key-sorted
  `std::multimap` and re-serialises it, with no option to opt out, so these
  URLs go through `sharepoint_preauth.cpp` (raw TLS, verbatim target) instead.
  DuckDB 1.4's httplib 0.14 sent paths verbatim. The download host also accepts
  the request with or without the Graph bearer; we never send it there.
- **The download host is not reachable with a Graph token directly** —
  `/_api/v2.x/drives/.../content` on the tenant host is a 401 for a
  `graph.microsoft.com` audience token, so the redirect is unavoidable.
- **`Prefer: permanent-delete` is ignored** on SharePoint drives (the item goes
  to the recycle bin). Permanent deletion is `POST .../items/{id}/permanentDelete`.
- **DuckDB's COPY moves a `tmp_` file onto the final name**, so a move must send
  `?@microsoft.graph.conflictBehavior=replace`, or every re-run of a COPY is a
  409 `nameAlreadyExists`.
- **SharePoint forbids `" * : < > ? / \ |` in names** (Graph answers 400). JSON
  bodies are still built with picojson; the legal "hostile" set (`' # % + & ;
  { }`, spaces, non-ASCII) round-trips and is in the live suite.
- **DuckDB 1.5's Parquet reader calls `GetLastModifiedTime`**; it is served from
  `lastModifiedDateTime` (RFC 3339).

## 8. Two-actor note

The extension binary is built and released from **this** repo (ghcr / the
community-extensions registry). escurel consumes it purely as a config change:
`ESCUREL_STORAGE_DUCKVFS_ROOT=sharepoint://…`, a `sharepoint` secret, and the
built `sharepoint.duckdb_extension` path — no escurel code change once the
`duckvfs` config is generalised to scheme-agnostic secret params (a small,
separately-tracked escurel PR).
