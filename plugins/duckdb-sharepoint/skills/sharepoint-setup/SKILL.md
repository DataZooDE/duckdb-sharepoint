---
name: sharepoint-setup
description: >
  Installing and authenticating the sharepoint DuckDB extension: INSTALL/LOAD,
  CREATE SECRET (TYPE sharepoint) with PROVIDER client_credentials (app-only,
  client secret), client_certificate (app-only, PEM certificate),
  credential_chain (the Azure workload's own identity, no secret), device_code
  and authorization_code (as a user, refresh token kept in the secret), config
  (a token you already have); TENANT_ID, CLIENT_ID, SITE_URL (+ DRIVE) or DRIVE_ID / SITE_ID,
  sharepoint_sites() / sharepoint_drives() to find them,
  GRAPH_SCOPE (never SCOPE); which Graph permissions each feature needs
  (Sites.ReadWrite.All, Sites.Manage.All, Sites.Selected least privilege);
  national clouds; and what Entra/Graph errors mean (AADSTS700016,
  AADSTS7000218, AADSTS50173, HTTP 403 accessDenied). Use when connecting
  DuckDB to SharePoint/OneDrive, writing a CREATE SECRET, or fixing a sign-in
  or permission error.
version: 2026.11.13
user-invocable: false
---

# sharepoint extension — setup and sign-in

```sql
INSTALL sharepoint FROM community;
LOAD sharepoint;
```

## Secrets (one per drive; DuckDB picks the secret whose SCOPE prefix matches)

| PROVIDER | when | fields |
|---|---|---|
| `client_credentials` (default) | unattended, as an app | `TENANT_ID`, `CLIENT_ID`, `CLIENT_SECRET` |
| `client_certificate` | unattended, app with a certificate | `TENANT_ID`, `CLIENT_ID`, `CERTIFICATE_PATH` (PEM: cert + RSA key), optional `CERTIFICATE_PASSWORD` |
| `credential_chain` | in Azure, no secret: workload identity, then managed identity (App Service / IMDS), then `az` CLI | optional `CLIENT_ID` of a user-assigned identity |
| `device_code` | as yourself, any shell | `TENANT_ID`, `CLIENT_ID` (public client), optional `REFRESH_TOKEN` |
| `authorization_code` | as yourself, browser on this machine | `TENANT_ID`, `CLIENT_ID`, optional `REDIRECT_PORT` (8020) |
| `config` | a token you have | `ACCESS_TOKEN`, or `REFRESH_TOKEN` + `TENANT_ID` + `CLIENT_ID` |

Every provider also takes where `sharepoint://` paths hang off: `SITE_URL` (the site's address
from the browser; its default library, or `DRIVE '<library name>'`; a library's own address
picks that library), **or** `DRIVE_ID` (`b!...`) / `SITE_ID` (Graph ids); and optional
`GRAPH_SCOPE`. None is required for browser-address paths, `site :=` and discovery.

```sql
CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,
    TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '...', CLIENT_SECRET '...',
    SITE_URL 'https://contoso.sharepoint.com/sites/Sales');
CREATE PERSISTENT SECRET me (TYPE sharepoint, PROVIDER device_code,
    TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '...', DRIVE_ID 'b!...');
CREATE SECRET az (TYPE sharepoint, PROVIDER credential_chain, DRIVE_ID 'b!...');
```

`credential_chain` tries the sources in `DefaultAzureCredential`'s order and uses the
first token; when none has an identity, the error lists each source and why. Grant the
identity Graph's `Sites.ReadWrite.All` (or `Sites.Selected`) as an application permission.

Rules that bite:
- The OAuth scope option is **`GRAPH_SCOPE`**. `SCOPE` is DuckDB's path-prefix list for
  the secret; a scope URL there makes the secret match no path.
- Lists, search and other path-less functions use the secret in scope for `sharepoint://`
  (an unscoped one).
- Credentials go in the secret, never in queries; `getenv('...')` works in CREATE SECRET.
- A `PERSISTENT` delegated secret stores its refresh token on disk (redacted in
  `duckdb_secrets()`); Entra rotates it, the extension writes the newest back.

## Finding the site and library
```sql
SELECT name, web_url FROM sharepoint_sites();            -- or sharepoint_sites(search := 'Sales')
SELECT name, web_url, id FROM sharepoint_drives('https://contoso.sharepoint.com/sites/Sales');
```
`web_url` → `SITE_URL` / paths, `name` → `DRIVE`, `id` → `DRIVE_ID`. Work before the secret
names any drive. App-only `sharepoint_sites()` needs `Sites.Read.All` or `Sites.ReadWrite.All`.

## Permissions (Graph, admin consent)
- Files, Excel: `Files.ReadWrite.All` or `Sites.ReadWrite.All`.
- Lists: `Sites.ReadWrite.All`; creating lists (only fixtures do): `Sites.Manage.All`.
- Least privilege: `Sites.Selected` + a per-site grant
  (`POST /sites/{site-id}/permissions` with `{"roles":["write"],"grantedToIdentities":[{"application":{"id":"<app id>"}}]}`).
  A site not granted is refused (HTTP 403, or `site '…' not found`) with a hint naming
  Sites.Selected.
- Delegated: `Files.ReadWrite.All Sites.ReadWrite.All offline_access`.

## Errors
| error | fix |
|---|---|
| `AADSTS700016` | client ID and tenant from the same app's Overview page; use *Application (client) ID* |
| `AADSTS7000218` | *Authentication → Allow public client flows* → Enabled (device_code / authorization_code) |
| `AADSTS50173`, `invalid_grant` | refresh token expired: recreate the secret without `REFRESH_TOKEN` |
| HTTP 403 `accessDenied` | missing permission or consent, or (Sites.Selected) the site is not granted |
| `site '…' not found` | wrong site name, or (Sites.Selected) the site is not granted |

Settings: `sharepoint_graph_endpoint` / `sharepoint_login_endpoint` (national clouds; only
Microsoft hosts accepted), `sharepoint_block_size_bytes`, `sharepoint_upload_chunk_bytes`,
`sharepoint_immutable_prefixes`, `sharepoint_list_page_size`. Diagnostics:
`SELECT * FROM sharepoint_stats()` (Graph calls by kind), `CALL sharepoint_reset_stats()`,
`SELECT sharepoint_version()`.
