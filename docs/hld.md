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

Tokens are cached in-process keyed by **secret identity + drive** (never by
drive alone — one `FileSystem` object serves every `ClientContext`, so an
identity-blind cache would leak one tenant's token to another; this is the same
security note gdrive's `CacheKey` carries). A `config` secret with a bare
`ACCESS_TOKEN` is used as-is; with a `REFRESH_TOKEN` it uses the
`refresh_token` grant.

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
2. **Resumable upload session** (`PutContent`, large-file branch). The
   `createUploadSession` call is written; the chunked `PUT` loop against the
   returned absolute `uploadUrl` (own httplib client, no auth header,
   320-KiB-multiple chunks, resume-on-5xx via `expectedRanges`) is a TODO.
3. **Block cache** for reads (`sharepoint_block_size_bytes`), single-flight per
   block, identity-keyed — port from gdrive's `GDriveBlockCache`.
4. **Recursive `**` glob** across folders (currently one folder level).
5. **Live SQL suite** (`test/sql/live/`, `make test_live`) against a real
   tenant + a Python e2e harness (fixtures, API-call-count assertions), as
   gdrive has. This is the merge gate — no mocks.
6. **`authorization_code` / `device_code`** delegated providers via the
   `datazoo-oauth2` submodule.
7. **CI**: wire `MainDistributionPipeline.yml` to the DuckDB extension-ci-tools
   reusable workflow and add the community-extensions submission.

## 8. Two-actor note

The extension binary is built and released from **this** repo (ghcr / the
community-extensions registry). escurel consumes it purely as a config change:
`ESCUREL_STORAGE_DUCKVFS_ROOT=sharepoint://…`, a `sharepoint` secret, and the
built `sharepoint.duckdb_extension` path — no escurel code change once the
`duckvfs` config is generalised to scheme-agnostic secret params (a small,
separately-tracked escurel PR).
