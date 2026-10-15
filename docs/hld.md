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

## 8. Two-actor note

The extension binary is built and released from **this** repo (ghcr / the
community-extensions registry). escurel consumes it purely as a config change:
`ESCUREL_STORAGE_DUCKVFS_ROOT=sharepoint://…`, a `sharepoint` secret, and the
built `sharepoint.duckdb_extension` path — no escurel code change once the
`duckvfs` config is generalised to scheme-agnostic secret params (a small,
separately-tracked escurel PR).
