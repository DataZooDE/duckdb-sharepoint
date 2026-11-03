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
| write | streamed through a resumable session, published on close | same, over a `deferCommit` **upload session** |

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
as-is; with a `REFRESH_TOKEN` it uses the `refresh_token` grant
(`CLIENT_SECRET` optional — a public-client token refreshes without one).

### 5.1 Delegated (user) auth — `device_code` / `authorization_code`

Both providers are **public clients** of the Microsoft identity platform v2: an
Entra app with *Allow public client flows* enabled (device code) and/or an
`http://localhost` *Mobile and desktop applications* redirect (authorization
code; Entra ignores the port on a localhost redirect), with the delegated
`Files.ReadWrite.All`, `Sites.ReadWrite.All` and `offline_access` permissions
consented. There is no `CLIENT_SECRET` option. The scope defaults to
`<graph>/Files.ReadWrite.All <graph>/Sites.ReadWrite.All offline_access` for
the configured Graph cloud (`GRAPH_SCOPE` overrides; `offline_access` is what
makes Entra issue a refresh token).

```
device_code         POST {login}/{tenant}/oauth2/v2.0/devicecode  client_id & scope
                    -> print ONE line: verification_uri + user_code (stderr)
                    POST .../token  grant_type=urn:ietf:params:oauth:grant-type:device_code
                    every `interval` s (5 if absent; +5 on slow_down -- RFC 8628),
                    until success / expired_token / declined / sharepoint_device_code_timeout_seconds
authorization_code  PKCE S256 + state, browser, loopback server on REDIRECT_PORT (8020)
                    POST .../token  grant_type=authorization_code & code_verifier
```

Like gdrive, `CREATE SECRET` only stores configuration; the sign-in runs at
**first use** (so SQL never blocks at creation and creation is testable
offline). It needs a client connection (it writes the result back); an ATTACH
opener without one gets a teaching error. Afterwards the **refresh token is
written back into the secret** through the secret manager (on disk for a
`PERSISTENT` secret, redacted in `duckdb_secrets()`), and the access token is
seeded into the token cache; only the refresh token is persisted, never a
bearer token. From then on the secret is an ordinary refresh-token secret:
`MintingTokenSource` → `MintToken` → `grant_type=refresh_token` with no client
secret, exactly the `config` path. `REFRESH_TOKEN` can be passed at creation to
skip the sign-in (that is how the live suite and CI drive it).

**Write-back from inside a statement is intentional**, and guarded: the
sign-in can wait minutes for a human, so the secret is re-fetched by name and
storage first, and written only if its configuration (every field but
`refresh_token` / `refresh_token_lineage`, the provider and the SCOPE paths)
still equals the snapshot the statement started from and its refresh token is
still the one expected. A secret replaced or dropped meanwhile is left alone
(the statement still uses its tokens). Verified locally (2026-10-14): a write
from `GetAuthContext` is visible to the next open in the same statement and
survives a restart for a persistent secret, with no catalog deadlock.

**No side cache of sign-ins.** The sign-in is serialised per secret (name,
storage, provider, tenant, client id, scope, login host); a thread that waited
re-reads the secret manager and finds the refresh token the first one stored,
so a parallel scan prints one code. A secret recreated without
`REFRESH_TOKEN` — the recovery the "rejected" message teaches — really signs
in again, and a same-named secret for another user inherits nothing.

**Cache identity and rotation.** Entra issues a new refresh token on every
refresh grant. The credential fingerprint keys on the token's **lineage** —
SHA-256 of the refresh token the secret was first given (at creation or
sign-in), stored as the redacted `refresh_token_lineage` beside the token —
not on the current token, so rotation does not orphan the token cache, the
mint lock or the block-cache/same-drive identity. `MintToken` remembers the
newest rotated token per fingerprint (every later mint presents it, never a
superseded one), and `GetAuthContext` writes it back through the same guarded
write-back (best effort: a statement never fails because of it). A secret thus
stays usable past the first token's 90-day window as long as it is used.
Transport: the `/devicecode` and code-redemption POSTs go through the raw TLS
transport (`sharepoint_preauth.cpp`: one deadline, SNI + certificate-hostname
verification armed or fail closed, bounded body). The refresh grant uses the
existing `PostToken` (httplib, no redirects) shared with client credentials.

Reuse: `datazoo-oauth2` is a submodule pinned at the same commit as gdrive
(`91c18f9`). Its loopback server, browser launcher, PKCE/state generators and
`BuildAuthorizationUrlPure` are used as-is; its `OAuth2FlowV2::ExecuteFlow` is
not, because it redeems the code over its own httplib client (outside the
pre-auth transport rules) and it has no device-code grant — RFC 8628 is not in
the library. The pure parts (request bodies, reply classification, polling
interval, messages) are `sharepoint_delegated.cpp`, covered by Catch2.

Errors are classified from Entra's `error` + first `error_codes` entry, never
the description: `authorization_pending`/`slow_down` keep polling;
`expired_token`, `authorization_declined`/`access_denied`, AADSTS65001
(consent), AADSTS7000218 (public client flows disabled), `invalid_grant`
(refresh token expired/revoked → sign in again), `invalid_client`/`invalid_scope`
each get a message naming the fix.

### 5.2 Refresh-token rotation for CI (`RotateRefreshToken.yml`)

The live CI job drives the delegated suite with the repo secret
`SHAREPOINT_USER_REFRESH_TOKEN` (a test user's token from
`scripts/delegated_login.py`). Entra rotates the token on every redemption and
the extension rotates it in-process, but a job cannot write a repo secret, so
the stored one would age out (~90 days unused, earlier on a policy revocation).

`.github/workflows/RotateRefreshToken.yml` runs weekly (Mon 04:23 UTC) and on
`workflow_dispatch` (with a `dry_run` input). It runs
`scripts/rotate_refresh_token.py`, which:

1. validates its inputs (the shared `delegated_login.endpoints()`), then
   redeems the token at `{login}/{tenant}/oauth2/v2.0/token`,
   `grant_type=refresh_token`, public client (no secret), scope
   `<graph>/Files.ReadWrite.All <graph>/Sites.ReadWrite.All offline_access`,
   over the same no-redirect POST as the login script; a reply that is not a
   JSON object is treated as a failure;
2. emits `::add-mask::<new token>` before any other line, checks the token's
   shape (the login script's charset), and prints only SHA-256 fingerprints;
3. writes it with `gh secret set SHAREPOINT_USER_REFRESH_TOKEN --repo …`, the
   token on **stdin** (never argv), `GH_TOKEN` = `SECRETS_ROTATION_TOKEN`. The
   `gh` child's environment is an **allowlist** (`PATH`, `HOME`, locale,
   `TMPDIR`, CA and proxy variables, plus `GH_TOKEN`/`GH_PROMPT_DISABLED`), so
   no runner token, OIDC request token or tenant credential reaches it.

It fails loudly on `invalid_grant` (the token is dead: run
`make delegated_login` as the test user, then
`gh secret set SHAREPOINT_USER_REFRESH_TOKEN --repo DataZooDE/duckdb-sharepoint`
with the token on stdin), and up front when `SECRETS_ROTATION_TOKEN` is
missing. `--dry-run` redeems and validates but writes nothing (no PAT needed).
Offline tests: `e2e/tests/test_rotate_refresh_token_script.py` (stubbed POST,
stubbed and fake-executable `gh`, the CLI as a subprocess), run with the login
script's tests by the credential-free `scripts-offline` job in `Checks.yml`, so
fork PRs cover them too.

**Where the credentials live, and why.** `workflow_dispatch` can target any
branch, and the job runs that branch's copy of the script. A branch with a
modified script must never see the PAT, which can write *every* secret of the
repo. So:

| Secret | Where | Who can read it |
|---|---|---|
| `SECRETS_ROTATION_TOKEN` | **only** the `secrets-rotation` environment | jobs on `main` (environment deployment branches: `main` only) |
| `SHAREPOINT_USER_REFRESH_TOKEN`, `SHAREPOINT_TENANT_ID`, `SHAREPOINT_DELEGATED_CLIENT_ID` | repository secrets | the rotate job and the `Checks.yml` live job (which needs them) |

The rotate job sets `environment: secrets-rotation` and also guards on
`github.ref == 'refs/heads/main'` (defence in depth: a dispatch from another
branch is skipped, never rejected half-way). A dispatch with `dry_run: true`
does not put the PAT into the step's environment at all. The refresh token
stays a repo secret, so it is exactly as exposed to same-repo branches as it
was before this workflow (the live job already reads it); only the
secrets-write PAT is fenced off.

**`SECRETS_ROTATION_TOKEN`** — `GITHUB_TOKEN` can never write secrets, so the
workflow needs its own credential. A fine-grained PAT: GitHub → Settings →
Developer settings → Fine-grained tokens → Generate new token; *Resource
owner* `DataZooDE` (the org must allow fine-grained PATs, and may require
approval); *Expiration* as long as the org allows; *Repository access* → Only
select repositories → `duckdb-sharepoint`; *Repository permissions* →
**Secrets: Read and write** (Metadata: Read-only is added automatically);
nothing else. Store it in Settings → Environments → `secrets-rotation` →
Environment secrets (deployment branches: *Selected branches* → `main`), **not**
as a repository secret. A GitHub App installation token with the same single
permission on this one repository works equally.

| PAT record | |
|---|---|
| Owner | Joachim Rosskopf (the account that created it; its removal from the org kills the PAT) |
| Expires | *record the date here when the PAT is created or renewed* |

**Failure alerting.** A failed scheduled run would otherwise notify one
person at most. The `alert` job (`needs: rotate`, `if: failure()`, its own
`permissions: issues: write`, no secret but `GITHUB_TOKEN`) opens the issue
"Delegated refresh-token rotation failed", or comments on it if one is already
open, with the run link and the two usual fixes (`invalid_grant` → re-login;
`gh` 401/403 → PAT expired or under-scoped). The secrets-bearing job keeps
`contents: read`.

**Silent stops that alerting cannot see.**
- GitHub **disables scheduled workflows in a repository with no activity for
  60 days**; the weekly run then simply stops, so no run fails and no issue is
  opened. The token then dies ~90 days after the last redemption (the last
  rotation or live run). Re-enable the workflow in the Actions tab; any push
  to the repo resets the 60-day clock.
- An **expired PAT** does fail the run (gh 401) and is alerted, but only on the
  next weekly run; renew it before the date in the table above.

**Race with the live job, checked live (2026-10-14).** Entra does *not* revoke
a refresh token when it is redeemed: the same stored token, redeemed twice in a
row by `--dry-run`, succeeded both times and returned a different rotated token
each time. So a live job that read the secret before a rotation wrote it keeps
a working token, and the rotation never invalidates a token in flight. The
`rotate-refresh-token` concurrency group stops two rotations overlapping. What
does revoke every token of the lineage: a password reset, an admin "revoke
sessions", a Conditional Access change, or 90 days without any redemption.

## 6. Read / write / mutate (`sharepoint_client.*`, `sharepoint_filesystem.*`)

- **Read** — ranged `GET /items/{id}/content`. A shared block cache (as in
  gdrive, amortising per-request latency over larger blocks) is *declared*
  (`sharepoint_block_size_bytes`) but not yet implemented; reads are exact
  ranges for now.
- **Write** — streamed (see §7 item 6a): the handle holds at most one chunk;
  a single `PUT .../content` on `Close()` for files ≤
  `sharepoint_upload_chunk_bytes`, a `deferCommit` upload session fed chunk
  by chunk for larger ones. Positional writes and `Truncate`/`Trim` throw
  (Graph replaces whole files).
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
   each PUT carries a `Content-Range`; 202 advances, and a transport/5xx/429
   failure resumes from the session's `nextExpectedRanges` (`ParseRangeStart`,
   `QueryUploadSession`) for up to `kMaxUploadChunkRetries` attempts. The
   four pure helpers live in `sharepoint_upload.*` and are Catch2-tested; the
   PUT loop is in `sharepoint_client.cpp`. Since 2026-10-14 the session is fed
   by the streaming write path (6a), not by a whole-file buffer.
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
   `datazoo-oauth2` submodule. **Implemented (2026-10-14), §5.1.** Proven live
   so far: the real `/devicecode` request + RFC 8628 polling and the delegated
   refresh-error path (app-only app registration). *Open:* the end-to-end user
   round trip (`sharepoint_delegated.live.template`, `e2e/tests/test_delegated.py`)
   needs the `duckdb-sharepoint-delegated` app and a refresh token from
   `scripts/delegated_login.py`; the human device-code round trip is
   `e2e/tests/test_delegated.py::test_interactive_device_code_round_trip`
   (`SHAREPOINT_INTERACTIVE=1 ... pytest -s -k interactive`); an interactive
   `authorization_code` sign-in has only been exercised up to the browser
   launch. Rotated refresh tokens are written back (§5.1); a secret unused for
   ~90 days still needs a new sign-in. The CI repo secret
   `SHAREPOINT_USER_REFRESH_TOKEN` is rotated weekly by
   `RotateRefreshToken.yml` (§5.2), once `SECRETS_ROTATION_TOKEN` exists in
   the `secrets-rotation` environment.
6b. **DNS is not bounded by the pre-auth transport's deadline.** Connect,
   handshake, send and receive are; `getaddrinfo` inside `BIO_do_connect`
   blocks for the OS resolver timeout. Bounding it needs a resolver thread.
   *Open (crew round 4, F8; low).*
6a. ~~**Streaming writes.**~~ **Implemented and verified live 2026-10-14**
   (crew round 3, F6). The write handle holds a pure `ChunkedUploadBuffer`
   (`sharepoint_upload.*`, Catch2-tested) of at most one chunk. A file that
   never outgrows it is one `PUT` on close. The first byte past one chunk opens
   an upload session with **`deferCommit: true`** and every full chunk is `PUT`
   to the verbatim `uploadUrl` with `Content-Range: bytes a-b/*` as it fills;
   close sends the last chunk with the total, then `POST`s the empty-body
   commit, which is what publishes the file. A full chunk is sent only once
   MORE bytes arrive, so the final chunk is never empty and a file of exactly
   one chunk never opens a session. Per-chunk retry with the shared backoff,
   resuming from `nextExpectedRanges` (a `416` after a lost `202` is resynced
   the same way); a lost commit reply is recognised by size + changed eTag. A
   failed chunk or close, or a handle destroyed without a successful close (a
   `COPY` whose query errored mid-stream), `DELETE`s the session; a failed
   close throws. Counters: `upload_chunk`, `upload_commit`, `upload_abort`,
   high-water mark `upload_buffer_peak`. Measured: a 373 MiB `COPY` peaks at
   ~120 MiB RSS (local-disk `COPY` of the same: ~105 MiB), against ~520 MiB
   buffered. Live: `sharepoint.live.template` §4b, e2e
   `test_streaming_writes.py` (incl. the destination folder deleted
   mid-stream: the `COPY` fails with a 404 and nothing is left).
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

8. **Excel workbooks (#50, #52).** Read side implemented 2026-11-02:
   `read_excel_table` / `read_excel_range` (§9). Open: verified live in CI
   (`test/sql/sharepoint_excel.live.template`, `e2e/tests/test_excel_workbook.py`).
   Write side implemented 2026-11-02: `COPY ... (FORMAT excel_table|excel_range)`
   (§9.1); `OVERWRITE` replaces a table's rows / clears a range first
   (2026-11-02). 1904 date system detected (2026-11-03, #64).
9. **SharePoint lists (#49).** Read side implemented 2026-11-02:
   `read_sharepoint_list` (§10). Open: verified live (e2e, which needs the
   app's Sites.Manage.All to create its fixture lists once); multi-choice and
   person columns untested live; filter pushdown on indexed columns. Write
   side implemented 2026-11-02: `COPY ... (FORMAT sharepoint_list)` (§10.1).

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
- **`/devicecode` answers for a confidential app too** (2026-10-14): the
  app-only registration gets a code (`verification_uri`
  `https://login.microsoft.com/device`, `expires_in` 900, `interval` 5, numbers
  as JSON integers) and polling returns `authorization_pending` (AADSTS70016).
  That is what lets the live suite prove the device-code path without a human.
  A malformed refresh token is `invalid_grant` AADSTS9002313; a malformed device
  code `invalid_grant` AADSTS7000014.
- **App-only writes are attributed to a pseudo-user "SharePoint-App"**
  (`lastModifiedBy.user.displayName`), next to `application: Microsoft Graph`.
  "Has a `user`" therefore does not prove a delegated write; the e2e test
  checks the name.

Found while building streaming writes (2026-10-14):

- **An upload session refuses an unknown total — unless it is `deferCommit`.**
  `Content-Range: bytes 0-327679/*` is a `400 invalidRequest` ("Content-Range
  must include a total length") on a normal session, and accepted (`202`) on a
  session created with `"deferCommit": true`. With `deferCommit`, even the
  final chunk answers `202` with `nextExpectedRanges: []`; the item appears
  only on `POST <uploadUrl>` (empty body), which answers `201` (new) / `200`
  (replaced) with the driveItem. Non-final chunks need not even be 320-KiB
  multiples under `/*`, but we keep the rule.
- **A NEW file shows a 0-byte placeholder while its session is open** — `GET
  /root:/path` finds it (size 0) from `createUploadSession` on; a folder
  listing does not. `DELETE <uploadUrl>` (`204`) removes it. An EXISTING file
  keeps its old content and size until the commit, and is untouched by a
  discarded session.
- **DuckDB's own failure cleanup removes the COPY target.** A `COPY` that fails
  calls `TryRemoveFile` on every file it created; with `USE_TMP_FILE false`
  that is the real target, so an overwrite that fails mid-stream deletes the
  old file — exactly as on local disk. With the default tmp-file staging, the
  old file survives.

- **The `uploadUrl` is on the tenant's SharePoint host, and is allow-listed.**
  Observed live (2026-10-14, global cloud, app-only and delegated): a
  SharePoint document library's session URL is
  `https://<tenant>.sharepoint.com/[sites/<site>/]_api/v2.0/drives/<drive>/items/<item>/uploadSession?<signed query>`
  and a OneDrive for Business drive's is
  `https://<tenant>-my.sharepoint.com/personal/<user>/_api/v2.0/drive/items/<item>/uploadSession?<signed query>`
  -- https, no port, no userinfo, one host label in front of `sharepoint.com`.
  The query carries `guid`, `overwrite`, `rename`, `dc` and the `tempauth`
  credential. The chunk `PUT`s, the state `GET`, the commit `POST` and the
  discard `DELETE` (`204`) all go to that same URL. Since it is sent without a
  bearer and the URL alone authorises the write, `IsAllowedUploadUrl`
  (`sharepoint_identity.*`, Catch2-tested) refuses any URL off the cloud's
  SharePoint suffix before it is stored -- so no chunk, commit or `DELETE`
  can reach it -- with an
  `InvalidInputException` naming only the origin, never the query string.
  Rules: `https://`, an authority that is exactly `<label>.<suffix>` (label of
  letters, digits and `-`; so no userinfo, no port -- not even `:443` -- no IP
  literal, no trailing dot, no deeper subdomain), where the suffix follows
  `sharepoint_graph_endpoint`, per Microsoft's documented SharePoint Online
  endpoints:
  `graph.microsoft.com` -> `sharepoint.com`
  ([worldwide](https://learn.microsoft.com/en-us/microsoft-365/enterprise/urls-and-ip-address-ranges));
  `graph.microsoft.us` (GCC High) -> `sharepoint.us`
  ([GCC High](https://learn.microsoft.com/en-us/microsoft-365/enterprise/microsoft-365-u-s-government-gcc-high-endpoints));
  `dod-graph.microsoft.us` -> `sharepoint-mil.us`
  ([DoD](https://learn.microsoft.com/en-us/microsoft-365/enterprise/microsoft-365-u-s-government-dod-endpoints));
  `microsoftgraph.chinacloudapi.cn` -> `sharepoint.cn`
  ([21Vianet](https://learn.microsoft.com/en-us/microsoft-365/enterprise/urls-and-ip-address-ranges-21vianet)).
  Only the global cloud is verified live; the national suffixes are from those
  docs alone.
  **What is accepted, plainly: ANY tenant's host on the cloud's SharePoint
  suffix** -- `other-tenant.sharepoint.com` passes for a `contoso` drive (a
  Catch2 case records this). The check pins the cloud, not the tenant: it
  stops non-SharePoint hosts, other clouds, userinfo / port / IP-literal
  tricks and look-alike domains. That the label is *our* tenant rests on the
  uploadUrl arriving in a TLS reply from the allow-listed Graph host -- the
  same trust root as every other Graph answer. Exact-host pinning is **future
  work**: it needs a host source independent of that reply (the drive's
  `webUrl` comes from the same Graph reply, so it adds nothing), i.e. an
  admin-scoped setting, never a session setting.
  The sender's URL splitter (`SplitAbsoluteUrl` + `SplitHttpsOrigin`) and the
  allow-list both end the authority at the first `/`, `?` or `#`; a Catch2
  case asserts that for every allowed URL the sender reaches exactly the
  checked host on 443. The refusal text (`UploadUrlRefusalMessage`) is pure
  and Catch2-tested never to carry the path, query or userinfo. **Gap:** the
  refusal path in `GraphClient` itself has no live test -- with no mocks, a
  real tenant never hands out a non-matching uploadUrl, and pointing
  `sharepoint_graph_endpoint` at another cloud fails at token / Graph level
  before any session is opened. The DoD list also names `*.dps.mil`, which is not a tenant
  SharePoint host and is not allowed. **Custom (vanity) SharePoint domains are
  refused**: Graph hands out the tenant's `*.sharepoint.<cloud>` host for every
  drive we have seen, and allowing an arbitrary domain would defeat the check.
  A tenant that ever gets one sees a clear refusal (nothing is uploaded, the
  write fails) and it would need an explicit, admin-scoped opt-in -- not a
  session setting, which any SQL user can change. The `/content` download
  redirect is a read (no file content leaves the client) and is not covered.

Residuals of streaming writes, accepted and recorded (crew review 2026-10-14):

- **Discarding a session is one best-effort `DELETE`** (10 s timeout, one
  retry on transport error / 5xx), and it can run from the write handle's
  destructor while a failed `COPY` unwinds -- so a failed `COPY` can wait up
  to ~20 s for it. If it still fails, the session lingers until Graph expires
  it, and for a NEW file so does its 0-byte placeholder. `upload_abort` counts
  only discards that took.
- **A retried `createUploadSession` whose first reply was lost** may leave an
  orphaned session (and placeholder) behind. Not verified live.
- **Lost commit reply**: recognised by size + a changed **cTag** (the content
  tag); a concurrent writer of exactly the same size in that window is the
  remaining ambiguity. A 200/201 commit is success whatever its body says.
- **Memory**: the pending chunk is the only copy -- a chunk is written to the
  TLS socket straight from it (`BuildRawHttpHead` + body), never copied into a
  request string -- and it grows with what was written (a small file never
  allocates a whole chunk).
- **Seek on a write handle** to anywhere but the current position throws and
  marks the write failed (nothing is published). No DuckDB writer is known to
  do it; it is refused rather than allowed to desync the stream.


### Discovered on the first live runs of the workbook and list features (2026-11-02)

- **Excel for the web keeps a workbook it has opened locked.** A `DELETE` of
  the item right after workbook-API writes got HTTP 423 (`notAllowed`,
  "resource ... is locked"). The live suite therefore leaves its run folder to
  the nightly sweep (`LIVE_EXCEL_ROOT`, 24 h). An open session holds the lock
  longer, so a COPY that fails after `createSession` closes the session
  (`SessionGuard`).
- **`createSession` answers a transient HTTP 504 (`UnknownError`).** It is
  retried like a read; a duplicate session after a lost reply is harmless
  (it expires unused).
- **A missing workbook table is a 404 `ItemNotFound`**, with the message in
  the tenant's language ("Die angeforderte Ressource ist nicht vorhanden."),
  so tests match the name we put in the message, never Graph's text.
- **Read windows hold whole rows**: at 20,000 cells and 6 columns a window is
  3,333 rows, so 10,000 rows take four requests, not ⌈60,000 / 20,000⌉ = 3.
- **Creating a list needs `Sites.Manage.All`.** With only `Sites.ReadWrite.All`
  (application), `POST /sites/{id}/lists` is 403 `accessDenied`; reading and
  writing items is not. Only the e2e fixtures create lists.
- **Concurrent live runs share the suite's fixed paths.** CI concurrency is
  per branch, so several PRs run against the tenant at once; four re-runs
  started together made one run's `big.bin` disappear under another
  (404 `itemNotFound` on a read by id). Not a product bug -- stagger re-runs,
  and give new suites run-unique paths (`@RUN_ID@`), as the Excel suite does.
- **Delegated-app setup failures**, met recreating the public-client app for
  #47: `AADSTS700016` (client ID and tenant from different places -- take both
  from the app's Overview page), `AADSTS7000218` (*Allow public client flows*
  off; it must be switched on *and saved*), `AADSTS50173` (the stored refresh
  token expired; `make delegated_login` again).

## 8. Two-actor note

The extension binary is built and released from **this** repo (ghcr / the
community-extensions registry). escurel consumes it purely as a config change:
`ESCUREL_STORAGE_DUCKVFS_ROOT=sharepoint://…`, a `sharepoint` secret, and the
built `sharepoint.duckdb_extension` path — no escurel code change once the
`duckvfs` config is generalised to scheme-agnostic secret params (a small,
separately-tracked escurel PR).

## 9. Excel workbooks: `read_excel_table` / `read_excel_range` (#50)

Decided 2026-11-02 (#52): Excel before lists; **in this extension**, as table
functions beside `sharepoint://` over the same secret, auth and Graph client --
the filesystem's behaviour is untouched; **names as proposed in #50**.

`sharepoint://` already reads a saved `.xlsx` as bytes (the `excel`
extension's `read_xlsx` over the filesystem). These functions read the
**workbook API** instead -- what Excel for the web holds now, by table name or
address, without downloading the file -- which is what writing back (next
step) needs too.

- **Addressing.** The first argument is a `sharepoint://` path (scheme
  optional) or `id:<itemId>`, resolved with one stat to the item id; then
  `/drives/{d}/items/{id}/workbook/...`. Sessionless: each call opens the
  saved workbook. A non-persistent session would be faster for many windows
  but is one more call and a session to close; not needed yet.
- **Calls.** A table: `tables/{name}/headerRowRange?$select=values` (column
  names -- unique in an Excel table), `tables/{name}/dataBodyRange?$select=address`,
  then the rows. A range: the address as given, or for a bare sheet
  `worksheets/{s}/usedRange(valuesOnly=true)?$select=address`. Rows are read as
  `worksheets/{s}/range(address='A2:F5001')?$select=values,valueTypes,numberFormat`
  in windows of at most 20,000 cells (`kCellsPerRequest`): Graph rejects or
  times out on very large range requests. Counted as `workbook` in
  `sharepoint_stats()`; a small table is 1 stat + 3 workbook calls.
- **Whole read at bind.** Column types depend on every row, so bind reads the
  range and keeps the cells; the scan only converts. Fine for the sizes Excel
  is used at (tested at 10,000 rows); a sheet of millions of cells belongs in
  Parquet.
- **Types** (pure, `sharepoint_excel.*`, Catch2): booleans -> BOOLEAN; numbers
  by number format -- date-only -> DATE, date+time -> TIMESTAMP, time-only ->
  TIME, plain integral -> BIGINT, else DOUBLE, dates mixed with plain numbers
  -> DOUBLE; anything mixed with text -> VARCHAR. Dates are 1900-system
  serials (with Lotus' phantom 1900-02-29), rounded to the millisecond. Error
  cells (`#DIV/0!`) are NULL in a typed column. `all_varchar := true` reads
  text, dates as ISO text. A workbook in the 1904 date system (old Mac
  Excel) has serials 1462 days smaller; the workbook API does not say which
  system a workbook uses, so `xl/workbook.xml` is read for
  `<workbookPr date1904="1">` by three ranged reads (the zip's end record,
  its central directory, the entry), inflated through DuckDB's gzip decoder
  in a gzip frame (miniz itself is not exported to a loadable extension).
  Reads add the offset and writes take it off; anything unexpected in the
  zip means the 1900 system, never an error (#64).
- **Header.** A table's header row; for a range the first row unless
  `header := false`. Empty names become `column<N>`, duplicates get `_2`
  (case-insensitive, as DuckDB names are).

### 9.1 Writing back: `COPY ... (FORMAT excel_table | excel_range)`

```sql
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast');   -- append; create if missing
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_range, RANGE 'Out!B2');     -- overwrite from B2
```

- **In place, never by replacing the file.** Writes go through the workbook
  API inside one `createSession` (`persistChanges: true`) per COPY, so people
  can keep the workbook open in Excel. Nothing ever uploads or renames the
  `.xlsx`.
- **Why the public formats are a rewrite.** DuckDB's COPY TO an *existing*
  target defaults to `USE_TMP_FILE`: the writer is handed `tmp_<name>` and the
  operator `MoveFile`s it over the target at the end (bind_copy.cpp: the
  default is "file exists and not a known remote prefix" -- and
  `sharepoint://` is not one). For a workbook that rename would be our
  filesystem moving `tmp_plan.xlsx` over `plan.xlsx`. So `excel_table` /
  `excel_range` carry only `CopyFunction::plan` (the hook DuckDB's own JSON
  writer uses): it copies the statement, sets `USE_TMP_FILE false` plus a
  private marker option, points FORMAT at `__sharepoint_excel_table|range`,
  and calls `Binder::Bind` again. The internal writers refuse to bind without
  the marker (the binder strips `USE_TMP_FILE` itself before `copy_to_bind`),
  and an explicit `USE_TMP_FILE true` is refused. Same hook and option flow on
  1.5.6 and 2.0 (2.0's options are keyed by Identifier).
- **excel_range.** The block starts at RANGE's top-left cell (A1 for a bare
  sheet); a missing sheet is created (`worksheets/add`). `HEADER` (default
  true) writes the column names first. Rows go out as `PATCH .../range(...)`
  in windows of <= 20k cells, in query order (`REGULAR_COPY_TO_FILE`: one
  writer). Cells beyond what is written are left as they were.
- **excel_table.** An existing table: COPY columns are matched to the table's
  columns **by name** (case-insensitive, any order); one not in the table is
  an error that lists the table's columns, a table column not in the COPY
  stays empty. Rows go out as `POST tables/{name}/rows/add`. A missing table
  is created: on the sheet named by `SHEET` (default: the table's name),
  created if missing and refused if it already holds data, the header and
  rows written as a range, then `tables/add` + a rename.
- **Values.** Numbers and booleans as such; NULL and non-finite numbers clear
  the cell. DATE / TIMESTAMP / TIME go out as serials with a number format
  (`yyyy-mm-dd`, `yyyy-mm-dd hh:mm:ss`, `hh:mm:ss`) so they read back typed;
  for an appended table block the formats are a second PATCH over the new
  rows. Text is written with a leading apostrophe (`ExcelTextInput`): Graph
  sets values *as if typed*, so `=1+1` would become a formula and `00123` a
  number. Other types are written as their text. TIMESTAMPTZ is written in
  UTC. Excel keeps 15 significant digits.
- **OVERWRITE.** COPY's own `OVERWRITE` (a file option the binder would
  consume) is carried past it by the rewrite as a private option, so it never
  reaches DuckDB's file handling; `OVERWRITE_OR_IGNORE` and `APPEND` are
  refused. `excel_table` + OVERWRITE replaces an existing table:
  `tables/{name}/range` (its address, header included), `convertToRange`,
  `clear` (`applyTo: All`) of those cells, then the create path from the old
  top-left cell -- same name and place, the COPY's columns, Excel's default
  table style. `excel_range` + OVERWRITE first clears (`applyTo: Contents`,
  formatting kept) everything from the anchor down and right within the
  sheet's used range (`ExcelClearRegion`, pure), so a shorter rewrite leaves
  no stale rows.
- **Retries.** `rows/add`, `tables/add` and `worksheets/add` are not
  idempotent: they are re-sent only after throttling (429/503: not applied),
  never after a 5xx or a lost reply, which may have been applied and would
  append the rows twice (`IsRetryableWhenNotIdempotent`). A range PATCH is
  idempotent and retried like any read.

## 10. SharePoint lists: `read_sharepoint_list` (#49)

Decided 2026-11-02 (#52): in this extension, names as proposed in #49, after
the Excel functions.

```sql
SELECT * FROM read_sharepoint_list('Orders');                  -- the secret's site
SELECT * FROM read_sharepoint_list('id:4f2c...');              -- by list id
SELECT * FROM read_sharepoint_list('Orders', site := 'Sales'); -- /sites/Sales on the same host
```

- **Which site.** The secret names a drive (DRIVE_ID, or SITE_ID's default
  library), so the default site is the drive's: `GET /drives/{d}/list` ->
  `parentReference.siteId`. `site :=` takes a Graph site id (`host,guid,guid`),
  `host:/sites/x`, or a path on the same host (`Sales` = `sites/Sales`,
  `teams/Ops`). Lists have no path, so the secret used is the one in scope for
  `sharepoint://` (an unscoped one).
- **Calls.** `lists/{name|id}?$select=id` (Graph takes the list's name in
  place of its id), `lists/{id}/columns`, then
  `lists/{id}/items?$expand=fields&$top=999`, following `@odata.nextLink`.
  The page size is `sharepoint_list_page_size` (default 999); the e2e test
  pages a 300-item fixture 50 at a time instead of building a 5,000-item list:
  creating that many items is throttled to a crawl, and nextLink paging is the
  same at any size.
  Paging alone works past the 5,000-item list view threshold. Counted as
  `list` in `sharepoint_stats()`.
- **When the items are read, and WHERE pushdown (#61).** A choice column
  shown as check boxes (or with no `displayAs`) may hold several values, and
  only the data says whether it does; such a list is read at bind, as the
  Excel reads are, and nothing is pushed down. Every other list is read at
  init, after `pushdown_complex_filter` has turned the WHERE predicates on
  **indexed** columns into `$filter` (past 5,000 items Graph refuses a filter
  on a non-indexed column; the `Prefer: HonorNonIndexedQueriesWarningMayFailRandomly`
  header is unreliable and not used). DuckDB keeps every predicate, so a
  pushed clause only has to be never stricter than its SQL
  (`ODataClause`, pure): text `=`, `IN` and `LIKE 'x%'` (SharePoint compares
  case-insensitively, a superset); number `=`, `IN`, and ranges made
  inclusive; timestamp ranges widened to whole seconds. No `<>`, no text
  ranges, no booleans. A filter Graph refuses falls back to the full read.
  EXPLAIN shows it as `Graph $filter`. Expressions are walked with
  `ExpressionIterator` and expression types only: 2.0 made comparisons
  function expressions.
- **Columns** (pure, `sharepoint_list.*`): `id` first, `created` / `modified`
  last (UTC); every column neither hidden nor read-only, named by its display
  name, ContentType and Attachments left out. The system names win a clash
  (a user column "Created" becomes `Created_2`).
- **Types.** text -> VARCHAR, number -> DOUBLE, currency -> DECIMAL(18,4),
  boolean -> BOOLEAN, dateTime -> TIMESTAMP (Graph returns UTC; a date-only
  column is midnight in the *site's* time zone, so it can read as the evening
  before -- not corrected yet), choice -> VARCHAR or VARCHAR[], hyperlink ->
  the URL, calculated -> by its output type, other -> VARCHAR (JSON text).
  Lookup -> the display value plus `<name>_id` (`<internal>LookupId`). Person
  (#63) -> `<name>`, `<name>_email`, `<name>_id`, lists for
  `allowMultipleSelection`; Author/Editor -> `created_by*` / `modified_by*`
  after `modified`. Graph gives a person's id in the site's User Information
  List; for several people it may instead send `<internal>` as
  `[{LookupId, LookupValue, Email}]`, which is selected and used when
  present. Otherwise names and emails are looked up in that list
  (`lists/User Information List/items/{id}`, `$batch` of 20), at init and
  only for the columns read (projection pushdown). Best effort: a person
  missing from the list, or a refused call, leaves the name NULL. A field missing from an item (Graph omits empty ones) is
  NULL.

### 10.1 Writing: `COPY ... TO '<list>' (FORMAT sharepoint_list)`

```sql
COPY (SELECT 'A-17' AS Title, 120.0 AS Amount, 2 AS Customer_id) TO 'Orders' (FORMAT sharepoint_list);   -- create
COPY (SELECT id, Amount * 1.1 AS Amount FROM read_sharepoint_list('Orders') WHERE Status = 'open')
TO 'Orders' (FORMAT sharepoint_list);                                                                   -- update by id
```

- **Create or update, per row.** A row with an `id` updates that item
  (`PATCH items/{id}/fields`, only the columns given); a row without one -- no
  `id` column, or a NULL id -- creates an item (`POST items`). The COPY target
  is the list's name (or `id:<listId>`), `SITE` as for reading.
- **Columns** are matched like the Excel table writer: by result name, any
  case, or by internal name (`PlanListWrite`, pure). `created`, `modified` and
  a lookup's display column are skipped -- Graph sets them, and a lookup is
  set through `<name>_id` -- so a `read_sharepoint_list` result can be changed
  and written back as it is. Any other name the list lacks is an error that
  lists its columns.
- **Values.** NULL clears the field; dates and timestamps as RFC 3339 UTC;
  numbers and booleans as such; a lookup id as a string (as Graph takes it); a
  LIST as a multi-choice value with its `@odata.type` annotation.
- **Requests.** `$batch` calls of 20 (Graph's limit). Each part is checked on
  its own: throttled parts are re-sent; an update that failed with a 5xx too
  (idempotent); a create that failed with a 5xx is reported, not re-sent --
  it may have been applied, and re-sending would create the item twice. A
  batch holding any create is itself sent as non-idempotent.
- **In place.** The target is a list, not a file, but COPY's temp-file rename
  would still be attempted for a name that exists as a local file; the same
  rewrite as the Excel writers (`sharepoint_copy_rewrite.hpp`) rules it out.

## 11. File versions: `sharepoint_versions` and `?version=` (#66)

- **Addressing.** `sharepoint://<path or id:...>?version=<label>` (labels
  are SharePoint's `major.minor`, digits and one dot). `?` cannot occur in a
  SharePoint name, so the suffix never collides with a real path; any other
  `?` stays a glob character. A path rather than a named parameter, because
  then every reader takes it (`read_parquet`, `read_csv`, `read_blob`, ...).
- **Reading.** `GET /items/{id}/versions/{v}` for the size and time, then
  ranged reads of `/items/{id}/versions/{v}/content` through the same 302 ->
  pre-authenticated URL path as `/content`. Blocks are cached under
  `version:<v>` in place of the cTag: a version never changes. The
  immutable-prefix metadata cache is bypassed. `Glob` returns such a path
  as it is (DuckDB globs it, because of the `?`).
- **Read-only.** Writing to it, and `remove_file`, `move_file` and the
  directory calls on it, are refused: with the version stripped, the path
  names the current file, which a delete of `x?version=1.0` must not hit.
- **`sharepoint_versions(path)`.** `GET /items/{id}/versions`, paged; Graph
  lists the current version first, and its `path` is the plain one.

