# Exploratory testing log

Time-boxed, charter-driven sessions against a **real** Microsoft 365 tenant
(no mocks). Each confirmed bug got a failing automated test first (live SQL
template or `e2e/`), then a fix in its own PR. Findings about Graph itself are
also in [`hld.md`](hld.md) §7a.

## Session 2026-10-10

**Setup:** DuckDB v1.5.6 build of this repo; app-only (client credentials)
secret on a document library; scratch under `/_duckdb_sharepoint_e2e`, removed
afterwards. Sibling `duckdb-gdrive` exercised in parallel for the cross-check.

| # | charter | what was tried | outcome |
|---|---|---|---|
| 1 | Hostile names & paths | 244-char name; 20-level nesting; NFC write / NFD read; trailing space; `' # % + & ; { }`, non-ASCII; `" :` | All legal names round-trip (SharePoint normalises Unicode). **Bug:** `:` reached Graph as `root:` delimiter → "Resource not found for the segment 'root:'"; `"` → bare 400. **Fixed:** forbidden names refused up front with a clear message. |
| 2 | Large & odd files | 0 B, 1 B, every 320-KiB chunk boundary ±1, 64 MiB; md5 vs Graph's size | Byte-exact everywhere (resumable upload sessions included). Now permanent e2e tests. |
| 3 | Throttling | 16 processes × 150 `file_size` (2,400 Graph calls) | Every process was throttled 4–8× (429/503); **all calls succeeded** via the retry/backoff. **Finding:** 3 stats per absent-file `file_size` → **fixed** (open-first: 1 stat present / 2 absent; also in gdrive). |
| 4 | Out-of-band change mid-session | delete+recreate (new id); in-place overwrite (same id, new cTag); external add before glob | Always seen by the running process (block cache keyed by cTag; no listing cache). Under `sharepoint_immutable_prefixes` the rewrite is *not* seen — by design, pinned by a test. |
| 5 | Error quality & leaks | wrong secret, bad drive id, forbidden names, `id:..`; grep every output for the client secret, JWTs, `tempauth` | Clear, typed messages; nothing leaks from the extension. **Bug in the test harness:** pytest tracebacks printed the secret fixture → **fixed** (redacted repr) here and in gdrive. |
| 6 | Secret lifecycle | `DROP SECRET` mid-session; persistent secret across restart; recreate with wrong credentials | Correct errors; persistence works; a recreated secret never reuses the old token. |
| 7 | Interaction | httpfs loaded alongside; gdrive loaded alongside; `ATTACH` a `.duckdb` file on SharePoint; DuckLake `DATA_PATH` | httpfs fine. **Bug:** gdrive + sharepoint could not be loaded together → **fixed** (both). **Bug:** `ATTACH 'sharepoint://…'` failed (DuckDB 1.5 canonicalised the path to `sharepoint:/…`, then opened it without a client connection) → **fixed** (both extensions). DuckLake works (insert, update, time travel, compaction). |
| 8 | Scoped secrets | only a secret `SCOPE`d to a subfolder; two secrets on two drives | **Bug:** `write_blob` walked parents from the root and asked about a path no secret covers → **fixed** (bottom-up walk; same bug in gdrive, fixed). |

## Cross-check with duckdb-gdrive

One scenario script (write/size/overwrite, folder and absent handling,
idempotent remove, move onto existing, glob of folder / brackets / braces,
`read_csv` of an absent file, COPY re-run, append, missing secret) ran against
both extensions and the outcomes were diffed. Divergences found and fixed:

- `glob` of a folder path returned the folder (sharepoint) — files only now.
- `read_csv` of an absent file surfaced Graph's 400 from the literal-name
  fallback (sharepoint) — impossible names are no longer stat-ed.
- `file_size` of a folder returned `0` (gdrive) — folders are not files.
- `{a,b}` alternation with literal expansions matched nothing (gdrive) — fixed.

Remaining, intentional: the exception class for a missing secret
(`InvalidInputException` here, `IOException` in gdrive).
