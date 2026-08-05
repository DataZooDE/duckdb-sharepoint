# duckdb-sharepoint

A DuckDB extension that registers a **`sharepoint://`** filesystem over
**Microsoft Graph**, so any DuckDB path expression can address a file in a
SharePoint document library (or OneDrive) with no download or copy step.
Because DuckDB dispatches file access through its virtual filesystem,
everything layered on it inherits the scheme: `read_parquet`, `read_csv`,
`COPY`, `glob`, and table formats such as DuckLake whose `DATA_PATH` lives on
SharePoint.

It is the sibling of [`duckdb-gdrive`](https://github.com/DataZooDE/duckdb-gdrive)
and shares its design and its scheme-generic `write_blob` / `remove_file` /
`move_file` / `file_size` SQL functions. Its first consumer is
[escurel](https://github.com/DataZooDE)'s DuckDB-VFS `LaneStore`, which needs a
`sharepoint://` backing store with the same read/write/delete/rename surface it
already drives on `gdrive://`.

> **Status: scaffold.** The pure-logic layer (URI parsing, glob matching, error
> classification) is implemented and unit-tested. The Graph-facing layer
> (`sharepoint_auth.cpp`, `sharepoint_client.cpp`, `sharepoint_filesystem.cpp`)
> is written against the documented Graph v1.0 API but **has not been compiled
> or run against a live tenant** — there is no DuckDB submodule checked out yet.
> The resumable upload-session path for files larger than the chunk size is a
> marked TODO. See [`docs/hld.md`](docs/hld.md) for the design and the exact
> remaining work.

## Why SharePoint is simpler than Drive

Google Drive has **no path addressing** — resolving `a/b/c.parquet` costs one
`files.list` per segment, which is why `duckdb-gdrive` carries a substantial
path→id cache. Microsoft Graph addresses driveItems **by path** natively:

```
GET /drives/{drive-id}/root:/Finance/2026/actuals.parquet:/content
```

So this extension needs **no path cache**, and because SharePoint forbids two
items of the same name in one folder, it needs none of gdrive's
duplicate-sibling-name (`R-4`) disambiguation either.

## Quick start

```sql
INSTALL sharepoint FROM community;   -- once published; until then LOAD a local build
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

-- Glob a folder.
SELECT * FROM read_parquet('sharepoint://exports/parts/*.parquet');

-- Write a whole file. Writes are sequential; Graph replaces whole files, so a
-- positional write raises rather than corrupting.
COPY (SELECT * FROM t) TO 'sharepoint://reports/out.parquet' (FORMAT parquet);

-- Byte-exact round trip for any content, no CSV/Parquet in between.
SELECT write_blob('sharepoint://notes/readme.md', '# Title'::BLOB);
SELECT remove_file('sharepoint://notes/readme.md');
```

## Authentication

| provider | use | fields |
|---|---|---|
| `client_credentials` *(default)* | headless daemon / escurel | `TENANT_ID`, `CLIENT_ID`, `CLIENT_SECRET` |
| `config` | paste a pre-obtained token | `ACCESS_TOKEN` *(or `REFRESH_TOKEN` + `TENANT_ID`/`CLIENT_ID`/`CLIENT_SECRET`)* |

Common to both: `DRIVE_ID` **or** `SITE_ID` (the drive the paths hang off), and
optional `GRAPH_SCOPE` (defaults to `https://graph.microsoft.com/.default`).
Interactive delegated flows (`authorization_code` / `device_code`) are a
planned follow-up — they need the `datazoo-oauth2` submodule the sibling gdrive
uses.

> The option is `GRAPH_SCOPE`, **not** `SCOPE`: DuckDB's `CREATE SECRET` grammar
> reserves `SCOPE` for the secret's path-matching prefix list, so an OAuth scope
> named `scope` would silently rescope the secret to match no path at all. This
> is the same trap `duckdb-gdrive` documents for `DRIVE_SCOPE`.

## Building

```sh
git clone --recursive https://github.com/DataZooDE/duckdb-sharepoint
cd duckdb-sharepoint
make            # builds the extension + runs the Catch2 pure-logic tests
make test_live  # SQL round trip against a real tenant (needs env credentials)
```

## National clouds

Set `sharepoint_graph_endpoint` and `sharepoint_login_endpoint` for GCC-High
(`graph.microsoft.us` / `login.microsoftonline.us`) or 21Vianet.

## License

MIT © 2026 DataZoo GmbH.
