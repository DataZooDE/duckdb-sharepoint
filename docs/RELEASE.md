# Releasing, and submitting to community-extensions

Adapted from duckdb-gdrive's `docs/RELEASE.md`, which records why each gate
exists. `duckdb/community-extensions` builds the extension itself from a tagged
commit and runs **no live tests of ours**, so whatever is broken at tag time
ships.

## Before tagging

```bash
export VCPKG_ROOT=/home/jr/.local/share/vcpkg
export VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

make check_pin          # submodule on the shipped tag
make check_cstdint      # musl builds too, not just glibc
rm -rf build/release    # MANDATORY if the submodule moved (stale version stamp)
GEN=ninja make
make check_stamp        # artifact built for the DuckDB we target
SMOKE_LOADABLE_STRICT=1 make smoke_loadable   # loads into a STOCK duckdb; a skip fails
make smoke_static       # only platform libraries dynamically linked
make unit_test
make check_credentials
make check_vfs_parity   # shared VFS functions identical to duckdb-gdrive

# Live, against a real tenant (see .env.sharepoint.example for the variables)
make test_live
make e2e                # includes test_community_descriptor.py: hello_world runs as written
```

Then confirm **both** workflows are green on the exact commit you will tag:

```bash
gh run list --repo DataZooDE/duckdb-sharepoint --limit 4
```

`Checks` runs hygiene, musl, the artifact, and the live suite, including the
delegated suite, which needs the rotated `SHAREPOINT_USER_REFRESH_TOKEN`
(hld §5.2). `Main Extension Distribution Pipeline` builds v1.5.6 and the
DuckDB 2.0 line on every platform and gates the v1.5.6 deploy on the smoke
test.

## Versioning: CalVer, `vYYYY.MM.DD`

The tag is the release date, as in every DataZoo extension. The string that
`SELECT sharepoint_version()` returns is hardcoded. When HEAD is on a tag,
`make check_stamp` fails if the string and the tag disagree. Bump the string in
its own PR before tagging, and update all three of these:

* `src/sharepoint_version.cpp`
* the expected value in `test/sql/sharepoint.test`
* the length check in `test/cpp/test_pure.cpp`

`extension_config.cmake` computes the extension version itself, because
DuckDB's `duckdb_extension_generate_version()` quotes its match pattern
literally and never finds a tag.

```bash
git tag -a vYYYY.MM.DD -m "sharepoint YYYY.MM.DD" <merge commit of the bump PR>
git push origin vYYYY.MM.DD
```

The tag push deploys versioned binaries to get.erpl.io. Confirm the stamp on
what actually shipped:

```bash
curl -sf http://get.erpl.io/sharepoint/vYYYY.MM.DD/v1.5.6/linux_amd64/sharepoint.duckdb_extension.gz \
  | gunzip | tail -c 512 | strings -n 4
```

### Verify what actually shipped

The tag push deploys versioned binaries. Check the stamp of **every** platform
against S3 directly -- get.erpl.io is a CloudFront cache and can serve a stale
copy for a long time after a re-upload:

```bash
T=vYYYY.MM.DD; N=sharepoint; B=https://s3.eu-west-1.amazonaws.com/get.erpl.io
for v in v1.5.6; do for a in linux_amd64 linux_arm64 osx_amd64 osx_arm64 windows_amd64; do
  echo "$v $a: $(curl -sf $B/$N/$T/$v/$a/$N.duckdb_extension.gz | gunzip 2>/dev/null | tail -c 512 | strings -n 4 | grep -m1 -E '^v20')"
done; done
```

Every binary must print the tag (only v1.5.6 is deployed; it has no musl
build). On 2026-10-17 a `main` push of the release commit, built before
the tag existed, deployed after the tag run and overwrote gdrive v1.5.6 in the
`vYYYY.MM.DD` path with a binary stamped `v2026.10.16-5-gfccfa44`. Versioned
paths now come only from tag runs; if a stamp is still wrong, re-run the tag
run's `Deploy (<arch>)` jobs (one at a time) and ask for a CloudFront
invalidation of `/<name>/vYYYY.MM.DD/*`.

Then create the GitHub Release for the tag.

## Submitting

**The first submission is a one-off. Every later release is a bump PR**:
changed `ref`, `variegata` and `version` on a branch cut from `upstream/main`,
titled `sharepoint: bump to vYYYY.MM.DD`.

1. Copy `docs/community-extension-description.yml` to
   `community-extensions/extensions/sharepoint/description.yml`, dropping the
   staging header comment.
2. Set `repo.ref` and `repo.variegata` to the **tagged commit's full SHA**.
   Do not use the tag name or `main`. Look the SHA up **in this repo**: the
   community-extensions clone has no such tag, and an empty substitution still
   parses as valid YAML with a null `ref`. Require 40 hex characters. Check
   that `extension.version` matches the tag.
   - `ref` builds against DuckDB 2.0 (the community default since 2026-10-15),
     and `variegata` against v1.5.6; a missing key falls back to `ref`. One
     commit builds on both (duckdb-compat). There is no `andium` key: DuckDB
     1.4.x is not supported.
3. Open the PR from the `DataZooDE/community-extensions` fork.

## Excluded platforms, and why

`windows_amd64_mingw;wasm_mvp;wasm_eh;wasm_threads`. The extension is HTTPS
calls to Graph plus a loopback redirect server for interactive OAuth, and
neither works in a browser sandbox. A wasm side-module would build green and
then fail to LOAD. vcpkg's OpenSSL port is fragile on mingw.
