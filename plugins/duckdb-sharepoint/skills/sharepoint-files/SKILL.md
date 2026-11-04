---
name: sharepoint-files
description: >
  Files in SharePoint / OneDrive with the sharepoint DuckDB extension: the
  sharepoint:// filesystem (paths, id: form, globs with * ? [a-z] ** {a,b}),
  reading and writing Parquet/CSV/JSON/any reader through it, COPY TO,
  ATTACH of a .duckdb file, DuckLake DATA_PATH, write_blob / remove_file /
  move_file / file_size, earlier versions (?version=, sharepoint_versions),
  change feeds (sharepoint_changes with a delta token), a library's files with
  metadata (read_sharepoint_files), Microsoft Search (search_sharepoint), and
  who can see a file (sharepoint_permissions: grants, inherited, sharing links).
  Use when reading or writing files in SharePoint/OneDrive from SQL, syncing a
  folder, listing a library, finding a file, or reading an old version.
version: 2026.11.03
user-invocable: false
---

# sharepoint extension — files

Paths are relative to the secret's drive (document library):

```sql
SELECT * FROM 'sharepoint://Finance/2026/actuals.parquet';           -- one Graph call
SELECT * FROM read_csv('sharepoint://id:01ABCDEF...');                -- by driveItem id (rename-proof)
SELECT * FROM read_parquet('sharepoint://exports/**/part-[0-9]*.parquet');
COPY (SELECT * FROM t) TO 'sharepoint://reports/out.parquet' (FORMAT parquet);  -- streamed, appears on close
ATTACH 'sharepoint://warehouse/sales.duckdb' AS sales (READ_ONLY);
SELECT write_blob('sharepoint://notes/a.md', '# Hi'::BLOB);  -- remove_file / move_file / file_size too
```

- Reads are ranged and block-cached (`sharepoint_block_size_bytes`); folders under
  `sharepoint_immutable_prefixes` skip re-stat.
- Writes replace whole files; no append, no positional writes. A move across drives is refused.
- Names cannot contain `" * : < > ? \ |`.

## Library contents with metadata
```sql
SELECT name, path, size, modified, modified_by, "Department"
FROM read_sharepoint_files('sharepoint://Finance/**/*.xlsx');
```
Fixed columns `path, name, size, created, modified, created_by(_email), modified_by(_email),
content_type, id, web_url`, then the library's own columns, typed like list columns.

## Earlier versions
```sql
SELECT version, current, modified, modified_by, size, path FROM sharepoint_versions('sharepoint://f.parquet');
SELECT * FROM read_parquet('sharepoint://f.parquet?version=3.0');   -- any reader; read-only
```

## What changed since last time (delta)
```sql
SELECT * FROM sharepoint_changes('sharepoint://Finance/', token := 'latest');  -- just a token
SELECT * FROM sharepoint_changes('sharepoint://Finance/', token := '<token>');
```
Rows: `_change` = `upsert` | `delete` (by `id`; path may be NULL) | `checkpoint` (exactly one,
last; its `token` is for next time). No token = full listing. Expired token → start over.
Table functions take no subqueries: pass a stored token via `SET VARIABLE t = (...)` and
`getvariable('t')`.

## Who can see it (permissions)
```sql
SELECT path, granted_by, grantee_type, grantee, grantee_email, roles, link_scope, link_url, expires
FROM sharepoint_permissions('sharepoint://Finance/**');      -- a file, a folder, or a glob of files
```
One row per grantee; `granted_by` = `direct` | `inherited` | `link`; `link_scope` =
`anonymous` | `organization` | `users`. An org link nobody used yet has no grantee (NULL).

## Search
```sql
SELECT name, path, modified, summary FROM search_sharepoint('quarterly forecast');
SELECT name, web_url FROM search_sharepoint('Sales', types := ['site']);   -- or 'driveItem','listItem'
```
App-only search finds the tenant's region itself (or `region := 'EUR'`); `max_results`
(default 500). A hit in the secret's drive has a `sharepoint://` path that reads back. The
index lags new files by minutes.
