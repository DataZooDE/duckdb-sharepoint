# duckdb-sharepoint

A DuckDB extension that registers a **`sharepoint://`** filesystem over
**Microsoft Graph**, so any DuckDB path expression can address a file in a
SharePoint document library (or OneDrive) with no download or copy step.
Because DuckDB dispatches file access through its virtual filesystem,
everything layered on it inherits the scheme: `read_parquet`, `read_csv`,
`COPY`, `glob`, `ATTACH` of a DuckDB database file, and table formats such as
DuckLake whose `DATA_PATH` lives on SharePoint.

It is the sibling of [`duckdb-gdrive`](https://github.com/DataZooDE/duckdb-gdrive)
and shares its design, its glob syntax and its scheme-generic `write_blob` /
`remove_file` / `move_file` / `file_size` SQL functions. Both extensions load
into one database side by side.

> **Status:** verified end to end against a real Microsoft 365 tenant —
> the live SQL suite (`make test_live`) and the e2e harness (`make e2e`:
> out-of-band changes, restarts, 16-thread scans, DuckLake, two drives) pass.
> Built for DuckDB **v1.5.6** and the **v1.4.5 LTS** line.

## Why SharePoint is simpler than Drive

Google Drive has **no path addressing** — resolving `a/b/c.parquet` costs one
`files.list` per segment, which is why `duckdb-gdrive` carries a substantial
path→id cache. Microsoft Graph addresses driveItems **by path** natively:

```
GET /drives/{drive-id}/root:/Finance/2026/actuals.parquet:/content
```

So this extension needs **no path cache**, and because SharePoint forbids two
items of the same name in one folder, it needs none of gdrive's
duplicate-sibling-name disambiguation either.

## Installation

### From erpl.io (recommended)

Published to the erpl.io distribution bucket on every release. The binary is
**unsigned**, so start DuckDB with the `-unsigned` flag (or set
`allow_unsigned_extensions`):

```sql
-- duckdb -unsigned
INSTALL 'sharepoint' FROM 'http://get.erpl.io';
LOAD sharepoint;
```

### From the DuckDB community registry

```sql
INSTALL sharepoint FROM community;   -- once the community-extensions submission lands
LOAD sharepoint;
```

## Quick start

```sql
LOAD sharepoint;

-- Unattended app-only auth (the default). Register an app in Entra ID, grant it
-- the Sites.ReadWrite.All (or Files.ReadWrite.All) APPLICATION permission with
-- admin consent, then bind it to a document library by DRIVE_ID (or SITE_ID).
CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER client_credentials,
    TENANT_ID     '00000000-0000-0000-0000-000000000000',
    CLIENT_ID     '11111111-1111-1111-1111-111111111111',
    CLIENT_SECRET '...',
    DRIVE_ID      'b!AbCdEf...'          -- or SITE_ID 'contoso.sharepoint.com,<siteId>,<webId>'
);

-- Read a file by path (one Graph call — no per-segment walk).
SELECT count(*) FROM 'sharepoint://Finance/2026/actuals.parquet';

-- Or by driveItem id — stable across a rename.
SELECT * FROM read_csv('sharepoint://id:01ABCDEF23456789');

-- Glob: * ? [a-z] [!x] ** and {a,b} alternation, as in duckdb-gdrive.
SELECT * FROM read_parquet('sharepoint://exports/**/part-[0-9]*.parquet');

-- Write a whole file. Graph replaces whole files, so a positional write or an
-- append raises rather than corrupting.
COPY (SELECT * FROM t) TO 'sharepoint://reports/out.parquet' (FORMAT parquet);

-- Attach a DuckDB database file stored in SharePoint, read-only.
ATTACH 'sharepoint://warehouse/sales.duckdb' AS sales (READ_ONLY);

-- Byte-exact round trip for any content, no CSV/Parquet in between.
SELECT write_blob('sharepoint://notes/readme.md', '# Title'::BLOB);
SELECT remove_file('sharepoint://notes/readme.md');   -- true, then false
```

## Authentication

| provider | use | fields |
|---|---|---|
| `client_credentials` *(default)* | headless daemon | `TENANT_ID`, `CLIENT_ID`, `CLIENT_SECRET` |
| `config` | paste a pre-obtained token | `ACCESS_TOKEN` *(or `REFRESH_TOKEN` + `TENANT_ID`/`CLIENT_ID`, + `CLIENT_SECRET` for a confidential client)* |
| `device_code` | **as a user**, headless shells | `TENANT_ID`, `CLIENT_ID` *(optional `REFRESH_TOKEN`)* |
| `authorization_code` | **as a user**, browser on this machine | `TENANT_ID`, `CLIENT_ID` *(optional `REFRESH_TOKEN`, `REDIRECT_PORT`, default 8020)* |

Common to all: `DRIVE_ID` **or** `SITE_ID` (the drive the paths hang off;
a `SITE_ID` is resolved to the site's default library once and cached), and an
optional `GRAPH_SCOPE` (defaults to `<graph endpoint>/.default`, i.e. the right
cloud). Several secrets can coexist; DuckDB picks the one whose `SCOPE` path
prefix matches best, so different folders can map to different drives (moves
between two drives are refused rather than silently landing in one of them).

Tokens are cached per **fingerprint of the credentials** (never per secret
name), so a secret recreated under the same name with different credentials
authenticates afresh.

### Delegated (user) auth

`device_code` and `authorization_code` act **as the signed-in user** (files are
authored by them, and they see only what they may). Both are public clients —
no client secret. Register an Entra app with *Allow public client flows*
enabled (and, for `authorization_code`, an `http://localhost` *Mobile and
desktop applications* redirect), with the **delegated** permissions
`Files.ReadWrite.All`, `Sites.ReadWrite.All` and `offline_access` consented.

```sql
CREATE PERSISTENT SECRET sp_me (TYPE sharepoint, PROVIDER device_code,
    TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '...', DRIVE_ID 'b!...');
SELECT * FROM read_csv('sharepoint://Shared/report.csv');
-- sharepoint: to sign in, open https://login.microsoft.com/device and enter the code ABCD1234 (valid for 15 minutes)
```

The sign-in runs once, on first use; the refresh token is then stored in the
secret (on disk for a `PERSISTENT` one, redacted in `duckdb_secrets()`) and
refreshed silently from then on. `sharepoint_device_code_timeout_seconds`
bounds the wait. Pass `REFRESH_TOKEN` to skip the sign-in entirely, e.g. with
a token from `scripts/delegated_login.py` (`make delegated_login`), which runs
the device-code flow and writes the token to the git-ignored
`.env.sharepoint.delegated` — never to the terminal.

> The option is `GRAPH_SCOPE`, **not** `SCOPE`: DuckDB's `CREATE SECRET` grammar
> reserves `SCOPE` for the secret's path-matching prefix list, so an OAuth scope
> named `scope` would silently rescope the secret to match no path at all. This
> is the same trap `duckdb-gdrive` documents for `DRIVE_SCOPE`.

## Settings

| setting | default | meaning |
|---|---|---|
| `sharepoint_block_size_bytes` | 8 MiB | Reads are served from a shared, single-flight block cache (256 MiB, LRU, keyed by credentials + item + cTag). Capped at 64 MiB; `0` disables it. |
| `sharepoint_upload_chunk_bytes` | 10 MiB | Upload chunk size — and the most memory one file being written holds. Files up to this size upload in one PUT on close; larger ones stream through a resumable upload session, one chunk at a time, as they are written (a multiple of 320 KiB, below 60 MiB). `sharepoint_stats()` reports `upload_chunk`, `upload_commit`, `upload_abort` and the high-water mark `upload_buffer_peak`. |
| `sharepoint_permanent_delete` | `false` | `remove_file` sends items to the site's recycle bin unless this is set. |
| `sharepoint_immutable_prefixes` | *(empty)* | Comma-separated `sharepoint://` prefixes whose files are **never rewritten in place** (e.g. a DuckLake `DATA_PATH`). Their metadata is remembered, saving a Graph call per open. **Declaring a prefix whose files change causes stale reads.** |
| `sharepoint_device_code_timeout_seconds` | 900 | How long `PROVIDER device_code` waits for the user to sign in (1–3600; also capped by the code's own lifetime). |
| `sharepoint_graph_endpoint` / `sharepoint_login_endpoint` | global cloud | National clouds: `https://graph.microsoft.us` / `https://login.microsoftonline.us` (GCC High), `https://dod-graph.microsoft.us`, or the China cloud. Only these Microsoft hosts are accepted, so credentials cannot be redirected elsewhere. |
| `sharepoint_telemetry_enabled` | `true` | Anonymous usage telemetry, shared with the sibling DataZoo extensions. `false` stops it for the rest of the session; the one-time load event is sent before any setting can exist (DuckDB rejects a setting for an extension that is not loaded yet), so to suppress that too set the environment variable `DATAZOO_DISABLE_TELEMETRY=1`. |

## Diagnostics

```sql
CALL sharepoint_reset_stats();
SELECT * FROM read_parquet('sharepoint://exports/**/*.parquet');
SELECT * FROM sharepoint_stats();   -- token_request, stat, read_range, list_page,
                                    -- upload_*, retries, throttled, bytes_read, ...
                                    -- (upload_buffer_peak is a high-water mark)
```

Graph throttles SharePoint aggressively; every call is retried with jittered
exponential backoff (500 ms → 20 s, honouring `Retry-After`), and the
`throttled` / `retries` counters show it happening. Errors carry Graph's error
code (`HTTP 409 (conflict, nameAlreadyExists): ...`) and map to
`PermissionException` (403) or `InvalidInputException` (400) where that is what
they are.

## Limitations

- **Whole-file writes, streamed.** Graph replaces files; there is no
  positional write or append (both are refused). A write is **streamed**: the
  file being written holds at most one upload chunk in memory
  (`sharepoint_upload_chunk_bytes`), and anything larger goes up chunk by chunk
  through an upload session as it is written — a 373 MiB `COPY` peaks at
  ~120 MiB RSS for the whole process (it was ~520 MiB when writes were
  buffered). The session is opened with `deferCommit`, so the file is published
  only by the commit on close: an overwritten file keeps its old content until
  then, and a write that fails mid-stream — or a query that errors part-way
  through a `COPY` — discards the session and publishes nothing. While a NEW
  file's session is open, SharePoint shows a 0-byte placeholder at its path
  (removed if the session is discarded); `COPY` writes to a `tmp_` name and
  moves it into place, so the real name never shows it.
- **Names.** SharePoint forbids `" * : < > ? \ |` in file and folder names;
  such writes are refused up front.
- **ATTACH is read-only** (a DuckDB database file cannot be updated in place).
- **Delegated (user) auth** is not implemented yet.
- **Error class for a missing secret** is `InvalidInputException` here and
  `IOException` in `duckdb-gdrive`.

## Building and testing

```sh
git clone --recursive https://github.com/DataZooDE/duckdb-sharepoint
cd duckdb-sharepoint
GEN=ninja make      # extension + Catch2 binary
make test           # Catch2 pure-logic + credential-free SQL tests
make test_live      # SQL round trip against a real tenant
make e2e            # live end-to-end harness (Python + uv)
make check_pin check_credentials check_cstdint check_stamp smoke_loadable
```

Live tests read `SHAREPOINT_TENANT_ID`, `SHAREPOINT_CLIENT_ID`,
`SHAREPOINT_CLIENT_SECRET` and `SHAREPOINT_DRIVE_ID` from the environment (or a
git-ignored `.env.sharepoint`, see `.env.sharepoint.example`). There are no
mocks: everything Graph-facing is proven against a real tenant.

## License

MIT © 2026 DataZoo GmbH.
