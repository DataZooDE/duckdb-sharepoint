#!/usr/bin/env python3
"""Keep the CI user's refresh token alive (stdlib + the `gh` CLI).

Entra rotates a refresh token on every redemption, but the live CI job never
writes the new one back, so the repo secret SHAREPOINT_USER_REFRESH_TOKEN ages
out (~90 days without use, earlier if a policy revokes it). This script, run
weekly by .github/workflows/RotateRefreshToken.yml:

  1. redeems SHAREPOINT_USER_REFRESH_TOKEN at the tenant's v2.0 token endpoint
     as a PUBLIC client (no client secret), same scope as delegated_login.py;
  2. masks the rotated token for GitHub Actions (`::add-mask::`) before
     anything else is printed;
  3. writes it back with `gh secret set SHAREPOINT_USER_REFRESH_TOKEN`, the
     token on stdin (never argv), authenticated by SECRETS_ROTATION_TOKEN
     (an environment secret of `secrets-rotation`, main only), with an
     allowlisted child environment.

    python3 scripts/rotate_refresh_token.py --dry-run   # redeem + validate only
    python3 scripts/rotate_refresh_token.py             # also write the secret

Inputs (env): SHAREPOINT_TENANT_ID, SHAREPOINT_DELEGATED_CLIENT_ID,
SHAREPOINT_USER_REFRESH_TOKEN, and -- unless --dry-run -- SECRETS_ROTATION_TOKEN.
Never prints a token. On invalid_grant the stored token is dead: run
`make delegated_login` and update the secret by hand.

Tested offline by e2e/tests/test_rotate_refresh_token_script.py (stubbed post()
and gh runner).
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable

_spec = importlib.util.spec_from_file_location("delegated_login", Path(__file__).with_name("delegated_login.py"))
dl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dl)

SECRET_NAME = "SHAREPOINT_USER_REFRESH_TOKEN"
DEFAULT_REPO = "DataZooDE/duckdb-sharepoint"
REPO = re.compile(r"[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}")
RELOGIN = (f"the stored refresh token is no longer valid. Run `make delegated_login` as the test user, then "
           f"update the repo secret: gh secret set {SECRET_NAME} --repo {DEFAULT_REPO} "
           f"< <(. ./.env.sharepoint.delegated; printf %s \"${SECRET_NAME}\")")
PAT_HELP = ("SECRETS_ROTATION_TOKEN is not set: create a fine-grained PAT (or GitHub App token) limited to "
            f"{DEFAULT_REPO} with repository permission 'Secrets: Read and write', and store it as "
            "SECRETS_ROTATION_TOKEN in the 'secrets-rotation' environment (main only), not as a repo secret "
            "(see docs/hld.md §5.2)")

# The only variables the `gh` child inherits (plus GH_TOKEN, GH_PROMPT_DISABLED):
# no runner token, OIDC request token or tenant credential gets through.
CHILD_ENV_ALLOWLIST = ("PATH", "HOME", "LANG", "LC_ALL", "TMPDIR", "SSL_CERT_FILE", "SSL_CERT_DIR",
                       "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "NO_PROXY", "no_proxy")

GhFn = Callable[[list, str, dict], tuple[int, str]]


class RotateError(Exception):
    pass


def gh(argv: list, stdin: str, env: dict) -> tuple[int, str]:
    """Run gh with the secret on stdin; return (exit code, stderr)."""
    try:
        p = subprocess.run(argv, input=stdin, env=env, text=True, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, type(e).__name__
    return p.returncode, p.stderr


def fingerprint(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()[:12]


def _scrub(text: str, *secrets: str) -> str:
    for s in secrets:
        if s:
            text = text.replace(s, "***")
    return text[-500:]


def rotate(env: dict[str, str], dry_run: bool, repo: str = DEFAULT_REPO,
           post_fn: Callable[[str, dict], tuple[int, dict]] = dl.post, gh_fn: GhFn = gh,
           say: Callable[[str], None] = lambda s: print(s, flush=True)) -> None:
    """Redeem, validate and (unless dry_run) write back. Raises RotateError."""
    # Everything is validated before the first request.
    if not REPO.fullmatch(repo):
        raise RotateError("--repo must be OWNER/NAME")
    pat = env.get("SECRETS_ROTATION_TOKEN", "")
    if not dry_run and not pat:
        raise RotateError(PAT_HELP)
    try:
        _, client_id, base, scope = dl.endpoints(env)
    except dl.LoginError as e:
        raise RotateError(str(e)) from None
    old = env.get(SECRET_NAME, "")
    if not old:
        raise RotateError(f"set {SECRET_NAME}")
    if not dl.TOKEN.fullmatch(old):
        raise RotateError(f"{SECRET_NAME} has an unexpected shape (not an Entra refresh token)")

    status, body = post_fn(f"{base}/token", {
        "grant_type": "refresh_token",
        "client_id": client_id,
        "refresh_token": old,
        "scope": scope,
    })
    if not isinstance(body, dict):
        body = {}
    new = body.get("refresh_token")
    # Mask FIRST, before any other line can be printed: whatever the shape, the
    # runner must never show it. Only under Actions -- in a terminal the
    # workflow command would itself print the token.
    if isinstance(new, str) and new and env.get("GITHUB_ACTIONS") == "true":
        for part in new.splitlines():
            if part.strip():
                say(f"::add-mask::{part}")
    if status != 200 or not body.get("access_token"):
        if body.get("error") == "invalid_grant":
            raise RotateError(f"redeem failed: HTTP {status} ({dl.describe(body)}): {RELOGIN}")
        hint = ""
        if 7000218 in (body.get("error_codes") or []):
            hint = ": enable 'Allow public client flows' on the app registration"
        raise RotateError(f"redeem failed: HTTP {status} ({dl.describe(body)}){hint}")
    if not new:
        raise RotateError("no refresh token was returned -- is offline_access consented for the app?")
    if not isinstance(new, str) or not dl.TOKEN.fullmatch(new):
        raise RotateError("the returned refresh token has an unexpected shape; not writing it")

    rotated = new != old
    say(f"redeemed: refresh token {'rotated' if rotated else 'NOT rotated'} "
        f"(sha256 old={fingerprint(old)} new={fingerprint(new)})")
    if dry_run:
        say(f"dry run: {SECRET_NAME} not written ({'rotated' if rotated else 'unchanged'} token discarded)")
        return
    if not rotated:
        say(f"Entra did not rotate the token; {SECRET_NAME} left as is")
        return

    # The child sees the PAT as GH_TOKEN and no other credential.
    child = {k: env[k] for k in CHILD_ENV_ALLOWLIST if k in env}
    child.update({"GH_TOKEN": pat, "GH_PROMPT_DISABLED": "1"})
    code, err = gh_fn(["gh", "secret", "set", SECRET_NAME, "--repo", repo], new, child)
    if code != 0:
        raise RotateError(f"gh secret set failed (exit {code}): {_scrub(err, new, old, pat).strip()} -- "
                          "SECRETS_ROTATION_TOKEN must reach this repository with 'Secrets: Read and write'. "
                          "The old token is still valid; nothing is lost.")
    say(f"{SECRET_NAME} updated in {repo}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="redeem and validate, but do not write the secret")
    ap.add_argument("--repo", default=DEFAULT_REPO, help=f"OWNER/NAME (default {DEFAULT_REPO})")
    args = ap.parse_args()
    try:
        rotate(dict(os.environ), dry_run=args.dry_run, repo=args.repo)
    except RotateError as e:
        print(f"rotate_refresh_token: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
