<h1 align="center">duckdb-sharepoint</h1>
<p align="center"><strong>Microsoft 365 business data in SQL: files, Excel workbooks and SharePoint lists for DuckDB</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
  <a href="https://duckdb.org"><img src="https://img.shields.io/badge/DuckDB-1.5.6%20%7C%202.0-green.svg" alt="DuckDB 1.5.6 | 2.0"></a>
  <a href="https://duckdb.org/community_extensions/extensions/sharepoint"><img src="https://img.shields.io/badge/DuckDB-community%20extension-yellow.svg" alt="DuckDB community extension"></a>
  <a href="https://github.com/DataZooDE/duckdb-sharepoint/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/DataZooDE/duckdb-sharepoint/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=main" alt="Build"></a>
</p>

**Microsoft 365 business data in SQL.** Query files, Excel workbooks and
SharePoint lists in SharePoint and OneDrive directly from DuckDB, and write
results back where your team already works. There's no export step and no
copies to keep in sync.

```sql
SELECT * FROM 'sharepoint://Finance/2026/actuals.parquet';           -- files
SELECT * FROM read_excel_table('Finance/plan.xlsx', 'Sales');         -- Excel, live
SELECT * FROM read_sharepoint_list('Orders');                         -- SharePoint lists
COPY results TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast', OVERWRITE);   -- and back
```

| | read | write |
|---|---|---|
| **Files** in document libraries and OneDrive | `read_parquet`, `read_csv`, `read_json`, `glob`, `ATTACH`, DuckLake: anything on a `sharepoint://` path | `COPY … TO 'sharepoint://…'`, streamed |
| **Excel workbooks**, through the workbook API | `read_excel_table`, `read_excel_range` | `COPY … (FORMAT excel_table \| excel_range)`, in place |
| **SharePoint lists** | `read_sharepoint_list` | `COPY … (FORMAT sharepoint_list)`: create, or update by `id` |

One install, one secret, and app-only or user sign-in. Built for DuckDB
**v1.5.6** and **2.0**, by [DataZoo](https://data-zoo.de), and the sibling of
[duckdb-gdrive](https://github.com/DataZooDE/duckdb-gdrive) for Google Drive.

## Contents

- [Quick start](#quick-start)
- [Examples](#examples): forecasting, regression, data quality, lead scoring
- [Files: `sharepoint://`](#files-sharepoint)
- [Excel workbooks](#excel-workbooks)
- [SharePoint lists](#sharepoint-lists)
- [Installation](#installation) · [Authentication](#authentication) · [Settings](#settings) · [Diagnostics](#diagnostics) · [Limitations](#limitations)
- [Network and privacy](#network-and-privacy) · [How it works](#how-it-works) · [Building and testing](#building-and-testing)
- [Claude Code plugin](#claude-code-plugin) · [Support and feedback](#support-and-feedback) · [License](#license) · [Citation](#citation)

## Quick start

```sql
INSTALL sharepoint FROM community;
LOAD sharepoint;

-- Sign in as yourself: DuckDB prints a URL and a code to enter in a browser.
CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER device_code,
    TENANT_ID 'contoso.onmicrosoft.com',
    CLIENT_ID '11111111-1111-1111-1111-111111111111',   -- an Entra app, see Authentication
    DRIVE_ID  'b!AbCdEf...'                             -- or SITE_ID 'contoso.sharepoint.com,<siteId>,<webId>'
);

-- A Parquet file in the document library
SELECT count(*) FROM 'sharepoint://Finance/2026/actuals.parquet';

-- The "Sales" table of an Excel workbook, as it is right now
SELECT region, sum(revenue) FROM read_excel_table('Finance/plan.xlsx', 'Sales') GROUP BY ALL;

-- A SharePoint list, one row per item
SELECT Status, count(*) FROM read_sharepoint_list('Orders') GROUP BY ALL;
```

For unattended jobs, use `PROVIDER client_credentials` with an app secret.
[Authentication](#authentication) covers both.

## Examples

Each example is one SQL file and a small synthetic data set. Each reads
Microsoft 365 data live, runs a DataZoo extension on it, and writes the result
back.

| example | from | with | back to |
|---|---|---|---|
| [**Sales forecast**](examples/excel-forecast/) | Excel table of monthly sales | [anofox_forecast](https://github.com/DataZooDE/anofox-forecast): AutoETS, 12 months ahead per product | a `Forecast` table in the same workbook |
| [**Marketing-mix regression**](examples/excel-statistics/) | Excel table of spend and sales | [anofox_statistics](https://github.com/DataZooDE/anofox-statistics): OLS with CIs and p-values | a `Model` sheet in the same workbook |
| [**PII and data quality**](examples/list-data-quality/) | SharePoint list of customers | [anofox_tabular](https://github.com/DataZooDE/anofox-tabular): PII scan, email/phone/VAT/IBAN checks | an `Issues` column on every item, plus a masked Parquet copy |
| [**Lead scoring**](examples/list-lead-scoring/) | SharePoint list of leads | [anofox_tabfm](https://github.com/DataZooDE/anofox-tabfm): zero-shot classification, no training | `Prediction` and `Score` on every open lead |

The forecast, end to end:

```sql
CREATE TABLE sales AS SELECT * FROM read_excel_table('Finance/sales.xlsx', 'Sales');     -- live from the workbook
CREATE TABLE forecast AS                                                                -- anofox_forecast
SELECT product, ds AS month, yhat AS forecast, yhat_lower AS low, yhat_upper AS high
FROM ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo');
COPY forecast TO 'Finance/sales.xlsx' (FORMAT excel_table, TABLE 'Forecast', OVERWRITE); -- back, as an Excel table
```

## Files: `sharepoint://`

The extension registers a `sharepoint://` filesystem. DuckDB sends all file
access through its virtual filesystem, so everything built on it accepts the
scheme: `read_parquet`, `read_csv`, `read_json`, `COPY`, `glob`, `ATTACH` of a
DuckDB database file, and table formats such as DuckLake with their
`DATA_PATH` on SharePoint.

```sql
-- By path: one Graph call, with no folder-by-folder walk.
SELECT count(*) FROM 'sharepoint://Finance/2026/actuals.parquet';

-- By driveItem id: stable across a rename.
SELECT * FROM read_csv('sharepoint://id:01ABCDEF23456789');

-- Glob: * ? [a-z] [!x] ** and {a,b} alternation, as in duckdb-gdrive.
SELECT * FROM read_parquet('sharepoint://exports/**/part-[0-9]*.parquet');

-- Write a file. It is streamed in bounded memory and appears when complete.
COPY (SELECT * FROM t) TO 'sharepoint://reports/out.parquet' (FORMAT parquet);

-- A DuckDB database file stored in SharePoint, read-only.
ATTACH 'sharepoint://warehouse/sales.duckdb' AS sales (READ_ONLY);

-- Byte-exact round trip for any content, with no CSV or Parquet in between.
SELECT write_blob('sharepoint://notes/readme.md', '# Title'::BLOB);
SELECT remove_file('sharepoint://notes/readme.md');   -- true, then false
```

**What changed since last time.** `sharepoint_changes` reads Graph's delta
for a folder: one row per file or folder added or changed (`_change =
'upsert'`) or deleted (`'delete'`, by `id`; a delete may come without its
path), then one `'checkpoint'` row whose `token` is what to pass next time.
The token is yours to keep, e.g. in a table:

```sql
CREATE TABLE sync AS SELECT token FROM sharepoint_changes('sharepoint://Finance/', token := 'latest')
                     WHERE _change = 'checkpoint';                      -- start from now
-- later:
SELECT * FROM sharepoint_changes('sharepoint://Finance/', token := (SELECT token FROM sync));
```

Without a token it lists everything under the folder once. An expired token
says so; start over without one.


**Earlier versions.** SharePoint keeps a file's version history, and
`?version=<label>` reads one of them with any reader; earlier versions are
read-only:

```sql
SELECT version, current, modified, modified_by, size, path
FROM sharepoint_versions('sharepoint://Finance/actuals.parquet');
SELECT * FROM read_parquet('sharepoint://Finance/actuals.parquet?version=3.0');
```

`write_blob`, `remove_file`, `move_file` and `file_size` don't depend on the
scheme, so they work for any filesystem DuckDB can reach (`s3://`, `gs://`,
local paths). They are shared with duckdb-gdrive, so both extensions can be
loaded into one database.

## Excel workbooks

`read_excel_table` and `read_excel_range` read an `.xlsx` in SharePoint or
OneDrive through the Microsoft Graph **workbook API**. They return the values
the workbook holds right now, without downloading the file.

```sql
-- A named Excel table: the header row gives the column names.
SELECT * FROM read_excel_table('Finance/plan.xlsx', 'Sales');

-- A range, or a sheet name for its used range.
SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input!A1:F200');
SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input', header := false);

-- By driveItem id, stable across a rename.
SELECT * FROM read_excel_table('id:01ABCDEF23456789', 'Sales');
```

- **Types.** Each column gets one type, inferred from its values and number
  formats: BOOLEAN, BIGINT, DOUBLE, DATE, TIMESTAMP, TIME, or VARCHAR when a
  column mixes kinds. Empty cells and error values such as `#DIV/0!` are NULL.
  `all_varchar := true` reads everything as text, with dates as ISO text.
- **Paths.** A `sharepoint://` path (the scheme may be left out) or `id:…`.
  Sheet names with spaces are quoted Excel-style: `'''My Sheet''!A1:B9'` in SQL.

Write results back with `COPY`. The workbook is changed in place through the
same API and never re-uploaded, so it can stay open in Excel while you write:

```sql
-- Append rows to a table, matching columns by name. A missing table is
-- created on a new sheet of its name (or SHEET '<name>').
COPY (SELECT month, forecast FROM fc) TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast');

-- Overwrite a block from its top-left cell, column names first (HEADER false: data only).
COPY (SELECT * FROM summary) TO 'Finance/plan.xlsx' (FORMAT excel_range, RANGE 'Summary!B2');
```

Add `OVERWRITE` to replace instead: a table's rows are replaced (same name,
same place), and a range is cleared from its top-left cell down and right
before it is written, so a shorter result leaves nothing stale behind. Either
way a re-run gives the same workbook:

```sql
COPY forecast TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast', OVERWRITE);
```

Dates, timestamps and times are written with a date format, so they read back
typed. Text stays text: `=1+1` doesn't become a formula, and `00123` doesn't
become a number.

## SharePoint lists

`read_sharepoint_list` reads a SharePoint list as a table, one row per item:

```sql
SELECT * FROM read_sharepoint_list('Orders');                  -- a list on the secret's site
SELECT * FROM read_sharepoint_list('id:4f2c…');                -- by list id
SELECT * FROM read_sharepoint_list('Orders', site := 'Sales'); -- another site on the same host
```

- **Columns** are named by their display names and typed: text, number,
  currency (DECIMAL), yes/no, date and time (TIMESTAMP, UTC), date only
  (DATE), and choice (VARCHAR, or VARCHAR[] for several choices).
- **Lookups and people:** a lookup column gives its value plus `<name>_id`; a
  person column gives `<name>`, `<name>_email` and `<name>_id` (lists when
  it allows several people). Created By and Modified By come as
  `created_by`, `created_by_email`, `created_by_id` and the same for
  `modified_by`. Names and emails are looked up in the site's user list, only
  when the query reads them.
- Every row also has **`id`, `created` and `modified`**. Lists of any size
  are read page by page, including past the 5,000-item list view threshold.
- **Filters on indexed columns run in SharePoint.** `WHERE` with `=`, `IN`,
  a range or `LIKE 'abc%'` on an indexed column is sent to Graph as a
  `$filter`, so a large list returns only the matching items (`EXPLAIN` shows
  it). Other predicates are applied by DuckDB after the read. Index a column
  under *List settings → Indexed columns*. A list with a multi-select choice
  column is always read whole.

Write items back with `COPY`. A row with an `id` updates that item, a row
without one creates a new item, and columns are matched by name:

```sql
COPY (SELECT 'A-17' AS Title, 120.0 AS Amount, 2 AS Customer_id) TO 'Orders' (FORMAT sharepoint_list);
COPY (SELECT id, Amount * 1.1 AS Amount FROM read_sharepoint_list('Orders') WHERE Status = 'open')
TO 'Orders' (FORMAT sharepoint_list);
```

`created`, `modified` and a lookup's display value are skipped (a lookup is
set through `<name>_id`), so a list read with `read_sharepoint_list` can be
changed and written back as it is.

### What's in a library: `read_sharepoint_files`

```sql
SELECT name, path, size, modified, modified_by, "Department", "Status"
FROM read_sharepoint_files('sharepoint://Finance/**/*.xlsx');
```

One row per file the path or glob matches (the same glob syntax as
`sharepoint://`): `path`, `name`, `size`, `created`, `modified`, `created_by`,
`modified_by` (and their `_email`), `content_type`, `id` and `web_url`, then
the library's own metadata columns, typed as `read_sharepoint_list` types a
list's.

## Search

`search_sharepoint` runs Microsoft Search across the tenant's files, list
items and sites, with what the signed-in user (or the app) may see:

```sql
SELECT name, path, modified, modified_by, summary
FROM search_sharepoint('quarterly forecast');                       -- files and list items
SELECT name, web_url FROM search_sharepoint('Sales', types := ['site']);
SET VARIABLE plan = (SELECT path FROM search_sharepoint('plan 2026') LIMIT 1);
SELECT * FROM read_excel_table(getvariable('plan'), 'Sales');   -- a table function takes no subquery
```

A file in the secret's drive comes with a `sharepoint://` path, so a result
reads straight back. App-only search must name the tenant's region; the
extension finds it on the first search, or takes `region := 'EUR'`.
`max_results` (default 500) caps the rows. Search indexes new content with a
delay of minutes.

## A site as a database: `ATTACH`

```sql
ATTACH 'sharepoint://sites/Sales' AS sales (TYPE sharepoint);    -- 'sharepoint://' = the secret's site
SHOW ALL TABLES;                                                  -- the site's lists
SELECT * FROM sales.Orders WHERE Status = 'open';                 -- read_sharepoint_list underneath
INSERT INTO sales.Orders (Title, Amount) VALUES ('A-18', 99);
UPDATE sales.Orders SET Status = 'closed' WHERE id = 17;
DELETE FROM sales.Orders WHERE Status = 'cancelled';
```

The site's visible lists are the tables of schema `main`, typed from their
column definitions (a multi-select choice column is `VARCHAR[]`). Reads go
through `read_sharepoint_list`, filter pushdown included. `INSERT` creates
items (columns left out keep SharePoint's defaults); `UPDATE` and `DELETE`
address items by `id`. Lists are read once per `ATTACH`: after changing a
list's columns in SharePoint, `DETACH` and `ATTACH` again. Creating or
altering lists, and `RETURNING`, are not supported.

## Installation

```sql
INSTALL sharepoint FROM community;
LOAD sharepoint;
```

The community repository serves DuckDB 2.0. Until its DuckDB v1.5.6 build is
published, v1.5.6 users can install from erpl.io. That binary is unsigned, so
start DuckDB with `-unsigned` (or set `allow_unsigned_extensions`):

```sql
-- duckdb -unsigned
INSTALL 'sharepoint' FROM 'http://get.erpl.io';
LOAD sharepoint;
```

## Authentication

| provider | use | fields |
|---|---|---|
| `client_credentials` *(default)* | unattended jobs, as an app | `TENANT_ID`, `CLIENT_ID`, `CLIENT_SECRET` |
| `client_certificate` | unattended jobs, as an app, with a certificate | `TENANT_ID`, `CLIENT_ID`, `CERTIFICATE_PATH` *(optional `CERTIFICATE_PASSWORD`)* |
| `device_code` | **as yourself**, any shell | `TENANT_ID`, `CLIENT_ID` *(optional `REFRESH_TOKEN`)* |
| `authorization_code` | **as yourself**, browser on this machine | `TENANT_ID`, `CLIENT_ID` *(optional `REFRESH_TOKEN`, `REDIRECT_PORT`, default 8020)* |
| `config` | a token you already have | `ACCESS_TOKEN` *(or `REFRESH_TOKEN` + `TENANT_ID`/`CLIENT_ID`, + `CLIENT_SECRET` for a confidential client)* |

**Common to all providers:**
- `DRIVE_ID` **or** `SITE_ID`: the document library paths hang off. A
  `SITE_ID` resolves to the site's default library, once.
- Optional `GRAPH_SCOPE`, which defaults to `<graph endpoint>/.default`, i.e.
  the right cloud.

Several secrets can coexist. DuckDB picks the one whose `SCOPE` path prefix
matches best, so different folders can map to different drives. A move
between two drives is refused rather than silently landing in one of them.
Lists have no path, so they use the secret in scope for `sharepoint://`.

**App-only.** Register an app in Entra ID and grant it the
`Sites.ReadWrite.All` **application** permission, with admin consent.
`Files.ReadWrite.All` is enough for files and Excel workbooks, but SharePoint
lists need `Sites.ReadWrite.All`:

```sql
CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,
    TENANT_ID '00000000-0000-0000-0000-000000000000', CLIENT_ID '...', CLIENT_SECRET '...',
    DRIVE_ID 'b!AbCdEf...');
```

**With a certificate.** Microsoft recommends certificates over client
secrets for app-only access: they don't get copied around as text, and they
expire on a schedule you set. Upload the certificate (public part) to the
app registration under *Certificates & secrets*, and point the secret at a
local PEM file holding the certificate and its RSA private key:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -subj "/CN=duckdb" -days 365
cat cert.pem key.pem > /etc/sp/app.pem && chmod 600 /etc/sp/app.pem   # upload cert.pem to Entra
```

```sql
CREATE SECRET sp (TYPE sharepoint, PROVIDER client_certificate,
    TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '...', CERTIFICATE_PATH '/etc/sp/app.pem',
    DRIVE_ID 'b!...');
```

The file is read each time the secret is used, so a renewed certificate at
the same path is picked up without a new secret. `CERTIFICATE_PASSWORD`
decrypts an encrypted key and is redacted like a client secret.

**Least privilege.** `Sites.ReadWrite.All` reaches every site in the
tenant. To limit an app to chosen sites, grant it the `Sites.Selected`
application permission instead (admin consent as usual), then give it each
site it may use. An admin does this once per site, e.g. with PnP PowerShell:

```powershell
Grant-PnPAzureADAppSitePermission -AppId '<CLIENT_ID>' -DisplayName 'duckdb' `
    -Site 'https://contoso.sharepoint.com/sites/Sales' -Permissions Write
```

or with Graph, as an app that holds `Sites.FullControl.All`:

```http
POST https://graph.microsoft.com/v1.0/sites/{site-id}/permissions
{ "roles": ["write"],
  "grantedToIdentities": [{ "application": { "id": "<CLIENT_ID>", "displayName": "duckdb" } }] }
```

`write` covers files, Excel workbooks and reading and writing list items.
Creating or deleting lists themselves needs `manage` or `fullcontrol`; the
extension never does either. Every call stays inside the drive and the sites
you name, so nothing needs tenant-wide rights. A site the app was not granted
is refused with HTTP 403 `accessDenied`.

**As a user.** `device_code` and `authorization_code` act as the signed-in
user: files are authored by them, and they see only what they may. Both are
public clients, with no client secret. Register an Entra app with these
settings:
- *Allow public client flows* enabled.
- For `authorization_code`, an `http://localhost` redirect under *Mobile and
  desktop applications*.
- The **delegated** permissions `Files.ReadWrite.All`, `Sites.ReadWrite.All`
  and `offline_access`, consented.

```sql
CREATE PERSISTENT SECRET sp_me (TYPE sharepoint, PROVIDER device_code,
    TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '...', DRIVE_ID 'b!...');
SELECT * FROM read_csv('sharepoint://Shared/report.csv');
-- sharepoint: to sign in, open https://login.microsoft.com/device and enter the code ABCD1234 (valid for 15 minutes)
```

The sign-in runs once, on first use. The refresh token is then stored in the
secret (on disk for a `PERSISTENT` one, redacted in `duckdb_secrets()`) and
refreshed silently from then on. `sharepoint_device_code_timeout_seconds`
bounds the wait. Pass `REFRESH_TOKEN` to skip the sign-in entirely, e.g. with
a token from `make delegated_login`, which writes it to the git-ignored
`.env.sharepoint.delegated` and never to the terminal.

> **What a delegated secret holds.** The refresh token acts as the user for
> everything the default scope covers: read and write on every file and site
> the user can reach. Narrow it with `GRAPH_SCOPE` (keep `offline_access`).
> A `PERSISTENT` secret stores the token in DuckDB's secret directory **in
> plain text**, protected only by file permissions; that is DuckDB's design
> for every persistent secret. Prefer a temporary secret on shared machines.
> Entra rotates the token on each refresh and the extension writes the newest
> one back. A token unused for about 90 days needs a new sign-in.

Tokens are cached per **fingerprint of the credentials**, never per secret
name, so a secret recreated under the same name with different credentials
authenticates afresh.

**Where the IDs come from.** All of them are on the app registration's
*Overview* page in the Entra admin center (*App registrations → All
applications → your app*): **Application (client) ID** is `CLIENT_ID`, and
**Directory (tenant) ID** is `TENANT_ID` (a verified domain such as
`contoso.onmicrosoft.com` works too). The *Object ID* is not used. `DRIVE_ID`
and `SITE_ID` are SharePoint's, not Entra's.

**When a sign-in fails**, Entra's error code says what to fix:

| error | meaning | fix |
|---|---|---|
| `AADSTS700016` | no app with this client ID in this tenant | copy both IDs from the same app's *Overview* page; use the *Application (client) ID*, not the *Object ID* |
| `AADSTS7000218` | the app is not a public client | *Authentication → Allow public client flows* → **Enabled**, then **Save** |
| `AADSTS50173` / `invalid_grant` | the refresh token expired or was revoked | sign in again (recreate the secret without `REFRESH_TOKEN`) |
| HTTP 403 `accessDenied` | the app lacks a permission, or (with `Sites.Selected`) this site | add it under *API permissions* and click **Grant admin consent** (it shows *Not granted* until then); or grant the site (see *Least privilege*) |
| `site '…' not found` (`itemNotFound`) | no such site at that path, or (with `Sites.Selected`, if Graph hides it) the app was not granted it | check the name; or grant the site (see *Least privilege*) |

> The option is `GRAPH_SCOPE`, **not** `SCOPE`: DuckDB's `CREATE SECRET`
> grammar reserves `SCOPE` for the secret's path-prefix list. An OAuth scope
> named `scope` would silently rescope the secret so that it matches no path.

## Settings

| setting | default | meaning |
|---|---|---|
| `sharepoint_block_size_bytes` | 8 MiB | Block size of the shared, single-flight read cache (256 MiB, LRU, keyed by credentials + item + cTag). At most 64 MiB; `0` disables it. |
| `sharepoint_upload_chunk_bytes` | 10 MiB | Upload chunk size, and the most memory one file being written holds. Smaller files go up in one PUT on close; larger ones stream through a resumable upload session (a multiple of 320 KiB, below 60 MiB). |
| `sharepoint_permanent_delete` | `false` | `remove_file` moves items to the site's recycle bin unless this is set. |
| `sharepoint_immutable_prefixes` | *(empty)* | Comma-separated `sharepoint://` prefixes whose files are **never rewritten in place** (e.g. a DuckLake `DATA_PATH`). Their metadata is remembered, saving a Graph call per open. **Declaring a prefix whose files change causes stale reads.** |
| `sharepoint_device_code_timeout_seconds` | 900 | How long `PROVIDER device_code` waits for the sign-in (1–3600, also capped by the code's own lifetime). |
| `sharepoint_graph_endpoint` / `sharepoint_login_endpoint` | global cloud | National clouds: `https://graph.microsoft.us` / `https://login.microsoftonline.us` (GCC High), `https://dod-graph.microsoft.us`, or the China cloud. Only Microsoft hosts are accepted, so credentials can't be redirected elsewhere. |
| `sharepoint_telemetry_enabled` | `true` | Anonymous usage telemetry, shared with the sibling DataZoo extensions. `false` stops it for the session. To suppress the one-time load event too, set the environment variable `DATAZOO_DISABLE_TELEMETRY=1`. |

## Diagnostics

```sql
CALL sharepoint_reset_stats();
SELECT * FROM read_parquet('sharepoint://exports/**/*.parquet');
SELECT * FROM sharepoint_stats();   -- token_request, stat, read_range, list_page, upload_*,
                                    -- workbook, list, retries, throttled, bytes_read, ...
```

Graph throttles SharePoint aggressively. Every call is retried with jittered
exponential backoff (500 ms → 20 s, honouring `Retry-After`), and the
`throttled` / `retries` counters show it happening. Requests that must not
run twice, such as adding rows to a table or creating list items, are
re-sent only when Graph refused them, never after an ambiguous failure.
Errors carry Graph's own error code, e.g. `HTTP 409 (conflict,
nameAlreadyExists): …`. They raise `PermissionException` for a 403 and
`InvalidInputException` for a 400.

## Limitations

- **Whole-file writes, streamed.** Graph replaces files, so positional writes
  and appends are refused. A write holds at most one upload chunk in memory;
  a 373 MiB `COPY` peaks at about 120 MiB RSS. The file is published only when
  it is closed: an overwritten file keeps its old content until then, and a
  write that fails part-way publishes nothing.
- **Names.** SharePoint forbids `" * : < > ? \ |` in file and folder names;
  such writes are refused up front.
- **ATTACH is read-only.** A DuckDB database file can't be updated in place.
- **Excel.**
  - Workbooks are read whole when the query is planned, which suits Excel's
    sizes; a sheet of millions of cells belongs in Parquet.
- **Lists.**
  - Only predicates on indexed columns are sent to SharePoint, and none for
    a list with a multi-select choice column.
  - A date-only column is read as a DATE on sites from UTC-10:00 to
    UTC+13:30. Graph doesn't say a site's time zone, so sites at UTC-11,
    UTC-12, UTC+13:45 and UTC+14 read such dates a day off.

## Network and privacy

Your data goes only between DuckDB and Microsoft:

| what | where | opt out |
|---|---|---|
| Files, workbooks, lists | Microsoft Graph (`graph.microsoft.com`, or your national cloud) | — |
| Sign-in and token refresh | Entra ID (`login.microsoftonline.com`, or your national cloud) | — |
| File downloads and upload chunks | your tenant's own SharePoint host, via pre-authenticated URLs Graph hands out | — |
| Telemetry | one anonymous `extension_loaded` event to PostHog EU: the extension's name, version and platform, and a random installation id | `SET sharepoint_telemetry_enabled = false`, or `DATAZOO_DISABLE_TELEMETRY=1` to stop the load event too |

Only Microsoft hosts are accepted as endpoints, so a setting can't send your
credentials elsewhere. Access tokens are held in memory only. Refresh tokens
are stored only in a `PERSISTENT` secret, in DuckDB's secret directory (see
[Authentication](#authentication)). Telemetry never includes your data,
paths, table or list names, or SQL.

## How it works

Google Drive has **no path addressing**: resolving `a/b/c.parquet` costs one
`files.list` per segment, which is why duckdb-gdrive carries a large
path→id cache. Microsoft Graph addresses items **by path** natively:

```
GET /drives/{drive-id}/root:/Finance/2026/actuals.parquet:/content
```

So this extension needs no path cache. SharePoint also forbids two items of
the same name in one folder, so there is no duplicate-name ambiguity to
resolve either.
- **Reads** are ranged and served from a block cache.
- **Writes** stream through a resumable upload session.
- **Excel and list writes** go through their APIs, never as file uploads.

The design, and the Graph behaviours found along the way, are in
[docs/hld.md](docs/hld.md).

## Building and testing

```sh
git clone --recursive https://github.com/DataZooDE/duckdb-sharepoint
cd duckdb-sharepoint
GEN=ninja make      # extension + Catch2 binary
make test           # Catch2 pure-logic + credential-free SQL tests
make test_live      # SQL suites against a real tenant
make e2e            # live end-to-end harness (Python + uv)
make check_pin check_credentials check_cstdint check_stamp smoke_loadable
```

There are no mocks: everything Graph-facing is proven against a real
Microsoft 365 tenant. The test tenant needs two apps: an **app-only** one
with the application permissions `Sites.ReadWrite.All` and
`Sites.Manage.All` (the e2e harness creates its own fixture lists once; the
extension itself never needs it), and a **public-client** one for the
delegated tests (see [Authentication](#authentication)). Live tests read `SHAREPOINT_TENANT_ID`,
`SHAREPOINT_CLIENT_ID`, `SHAREPOINT_CLIENT_SECRET` and `SHAREPOINT_DRIVE_ID`
from the environment, or from a git-ignored `.env.sharepoint` (see
`.env.sharepoint.example`).

The delegated live tests use a test user's refresh token from
`make delegated_login` (`.env.sharepoint.delegated`). In CI it is the repo
secret `SHAREPOINT_USER_REFRESH_TOKEN`. The weekly `RotateRefreshToken.yml`
workflow redeems it and writes the rotated token back using
`SECRETS_ROTATION_TOKEN`. That token is a fine-grained PAT for this repository
only, with "Secrets: Read and write", stored in the `secrets-rotation`
environment, which only `main` can use.

To check the token locally without writing anything, run this from the repo
root. The subshell keeps the token out of your shell:

```sh
(set -a; . ./.env.sharepoint.delegated; set +a; python3 scripts/rotate_refresh_token.py --dry-run)
```

It prints only SHA-256 fingerprints of the old and rotated tokens. If it
reports `invalid_grant`, sign in again with `make delegated_login` and update
the secret. Details: [docs/hld.md §5.2](docs/hld.md).

## Claude Code plugin

The repository ships a [Claude Code](https://claude.com/claude-code) plugin with four skills
that document this extension's SQL API, so Claude Code can write queries against SharePoint
from a plain request such as *"read the Orders list, open orders only, and write a forecast
back into the Sales workbook"*.

```
/plugin marketplace add DataZooDE/duckdb-sharepoint
/plugin install duckdb-sharepoint@duckdb-sharepoint
```

| Skill | Covers |
|---|---|
| `sharepoint-setup` | INSTALL/LOAD, `CREATE SECRET` for every provider (client secret, certificate, device code, browser sign-in, token), Graph permissions incl. Sites.Selected, settings, sign-in and permission errors |
| `sharepoint-files` | the `sharepoint://` filesystem and globs, COPY/ATTACH/DuckLake, `write_blob` and friends, `read_sharepoint_files`, `?version=` and `sharepoint_versions`, `sharepoint_changes`, `search_sharepoint` |
| `sharepoint-excel` | `read_excel_table`, `read_excel_range`, type inference, writing back with `COPY … (FORMAT excel_table / excel_range)` and `OVERWRITE` |
| `sharepoint-lists` | `read_sharepoint_list`, column types (lookups, people, dates), filter pushdown on indexed columns, `COPY … (FORMAT sharepoint_list)` |

The skills live in [`plugins/duckdb-sharepoint/`](plugins/duckdb-sharepoint/). To use a local
checkout instead of the marketplace, start Claude Code with
`claude --plugin-dir ./plugins/duckdb-sharepoint`.

## Support and feedback

- Design notes and the Graph behaviours found along the way: [docs/hld.md](docs/hld.md)
- Issues: [GitHub Issues](https://github.com/DataZooDE/duckdb-sharepoint/issues)

If `sharepoint` misbehaves, please [open an issue](https://github.com/DataZooDE/duckdb-sharepoint/issues).
That includes a sign-in that won't complete, a workbook or list that reads
wrong, or a write that doesn't land. SharePoint behaves differently with
tenant settings, permissions and throttling in ways we can't reproduce here,
so a report with your setup is the fastest path to a fix. Errors end with
that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a
small banner says the same. It never prints when output is piped, in notebooks
or in CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

## License

MIT © 2026 DataZoo GmbH. See [LICENSE](LICENSE).

## Citation

If you use this extension in research, please cite:

```bibtex
@software{duckdb_sharepoint,
  title  = {duckdb-sharepoint: Microsoft 365 Business Data in SQL for DuckDB},
  author = {Joachim Rosskopf and DataZoo GmbH},
  year   = {2026},
  url    = {https://github.com/DataZooDE/duckdb-sharepoint}
}
```
