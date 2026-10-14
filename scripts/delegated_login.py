#!/usr/bin/env python3
"""Device-code sign-in for the delegated live tests (stdlib only).

Runs the RFC 8628 device authorization grant against the Microsoft identity
platform v2 as a PUBLIC client (no client secret) and writes the resulting
refresh token to a git-ignored env file. The terminal shows ONLY the
verification URL and the user code -- never a token.

    SHAREPOINT_TENANT_ID=... SHAREPOINT_DELEGATED_CLIENT_ID=... \\
        python3 scripts/delegated_login.py            # or: make delegated_login

Writes (mode 0600, replacing the file):

    .env.sharepoint.delegated
        SHAREPOINT_TENANT_ID='...'
        SHAREPOINT_DELEGATED_CLIENT_ID='...'
        SHAREPOINT_USER_REFRESH_TOKEN='...'

Load it in the test shell with `set -a; source .env.sharepoint.delegated; set +a`.
The Entra app needs "Allow public client flows" enabled and delegated
Files.ReadWrite.All, Sites.ReadWrite.All and offline_access consented.

Optional: SHAREPOINT_LOGIN_ENDPOINT / SHAREPOINT_GRAPH_ENDPOINT for national
clouds, SHAREPOINT_DELEGATED_ENV_FILE to write elsewhere.

Tested offline by e2e/tests/test_delegated_login_script.py (stubbed post()).
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable

REPO_ROOT = Path(__file__).resolve().parents[1]

# Mirrors IsAllowedLoginEndpoint / IsAllowedGraphEndpoint in
# src/sharepoint_identity.cpp -- keep the lists in step.
ALLOWED_LOGIN_HOSTS = {
    "login.microsoftonline.com",
    "login.microsoftonline.us",
    "login.partner.microsoftonline.cn",
    "login.chinacloudapi.cn",
}
ALLOWED_GRAPH_HOSTS = {
    "graph.microsoft.com",
    "graph.microsoft.us",
    "dod-graph.microsoft.us",
    "microsoftgraph.chinacloudapi.cn",
}
# Mirrors IsAllowedVerificationUri / IsValidUserCode in src/sharepoint_delegated.cpp.
VERIFICATION_DOMAINS = ("microsoft.com", "microsoftonline.com", "microsoftonline.us", "microsoftonline.cn",
                        "chinacloudapi.cn")
USER_CODE = re.compile(r"[A-Za-z0-9-]{1,16}")
GUID = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
TENANT = re.compile(r"[A-Za-z0-9.-]{1,253}")
# Entra refresh tokens are base64url segments joined by '.'; anything else --
# quotes, whitespace, '$', backticks -- would be unsafe in a sourced file.
TOKEN = re.compile(r"[A-Za-z0-9._~*!-]{20,8192}")


class LoginError(Exception):
    pass


def https_base(url: str, allowed: set[str], what: str) -> str:
    parsed = urllib.parse.urlsplit(url.rstrip("/"))
    if (parsed.scheme != "https" or parsed.hostname not in allowed or parsed.path or parsed.port
            or parsed.username or parsed.query or parsed.fragment):
        raise LoginError(f"{what} must be https://<one of {sorted(allowed)}>")
    return f"https://{parsed.hostname}"


def allowed_verification_uri(uri) -> bool:
    if not isinstance(uri, str) or not uri.startswith("https://") or len(uri) > 512:
        return False
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F for c in uri):
        return False
    host = re.split(r"[/?#]", uri[len("https://"):], maxsplit=1)[0].lower()
    if not host or any(c in host for c in "@:[]"):
        return False
    return any(host == d or host.endswith("." + d) for d in VERIFICATION_DOMAINS)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """A token endpoint never redirects; following a 307/308 would re-POST the
    device code (a credential) to wherever it pointed."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def post(url: str, form: dict[str, str]) -> tuple[int, dict]:
    req = urllib.request.Request(url, data=urllib.parse.urlencode(form).encode(),
                                 headers={"Accept": "application/json"})
    try:
        with _OPENER.open(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}
    except (ValueError, urllib.error.URLError, OSError):
        return 0, {}


def describe(body: dict) -> str:
    """The short error and AADSTS code -- never error_description."""
    codes = body.get("error_codes") or []
    err = str(body.get("error", "error"))
    err = err if re.fullmatch(r"[a-z_]{1,64}", err) else "error"
    return err + (f" AADSTS{int(codes[0])}" if codes and str(codes[0]).isdigit() else "")


def write_env(out: Path, values: dict[str, str]) -> None:
    """Replace `out` atomically, mode 0600 from the first byte. Values are
    validated by the caller and single-quoted for `source`."""
    tmp = out.with_name(out.name + ".tmp")
    try:
        tmp.unlink()  # O_CREAT would keep an existing file's (possibly wider) mode
    except FileNotFoundError:
        pass
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        os.fchmod(f.fileno(), 0o600)
        f.write("# Written by scripts/delegated_login.py -- git-ignored, never commit.\n")
        for key, value in values.items():
            if "'" in value or "\n" in value:
                raise LoginError(f"refusing to write {key}: unsafe characters")
            f.write(f"{key}='{value}'\n")
    os.replace(tmp, out)


def _as_int(v, default: int) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def login(env: dict[str, str], post_fn: Callable[[str, dict], tuple[int, dict]] = post,
          sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic,
          say: Callable[[str], None] = lambda s: print(s, flush=True)) -> dict[str, str]:
    """Run the flow and return the values to write. Raises LoginError."""
    tenant = env.get("SHAREPOINT_TENANT_ID", "")
    client_id = env.get("SHAREPOINT_DELEGATED_CLIENT_ID", "")
    if not tenant or not client_id:
        raise LoginError("set SHAREPOINT_TENANT_ID and SHAREPOINT_DELEGATED_CLIENT_ID")
    if not TENANT.fullmatch(tenant) or tenant in (".", "..") or ".." in tenant:
        raise LoginError("SHAREPOINT_TENANT_ID must be a tenant GUID or domain")
    if not GUID.fullmatch(client_id):
        raise LoginError("SHAREPOINT_DELEGATED_CLIENT_ID must be the app's client id (a GUID)")
    login_host = https_base(env.get("SHAREPOINT_LOGIN_ENDPOINT", "https://login.microsoftonline.com"),
                            ALLOWED_LOGIN_HOSTS, "SHAREPOINT_LOGIN_ENDPOINT")
    graph = https_base(env.get("SHAREPOINT_GRAPH_ENDPOINT", "https://graph.microsoft.com"),
                       ALLOWED_GRAPH_HOSTS, "SHAREPOINT_GRAPH_ENDPOINT")
    scope = f"{graph}/Files.ReadWrite.All {graph}/Sites.ReadWrite.All offline_access"

    base = f"{login_host}/{tenant}/oauth2/v2.0"
    status, grant = post_fn(f"{base}/devicecode", {"client_id": client_id, "scope": scope})
    if status != 200 or not isinstance(grant.get("device_code"), str):
        raise LoginError(f"device-code request failed: HTTP {status} ({describe(grant)})")
    uri, code = grant.get("verification_uri"), grant.get("user_code")
    if not allowed_verification_uri(uri):
        raise LoginError("device-code response has no https Microsoft verification_uri")
    if not isinstance(code, str) or not USER_CODE.fullmatch(code):
        raise LoginError("device-code response has no valid user_code")

    # The ONLY thing printed: where to go and what to type.
    say(f"Open {uri} and enter the code {code}")

    interval = max(1, min(_as_int(grant.get("interval"), 5) or 5, 60))
    deadline = clock() + max(1, _as_int(grant.get("expires_in"), 900))
    while True:
        sleep(interval)
        if clock() >= deadline:
            raise LoginError("the device code expired before sign-in completed; run again")
        status, body = post_fn(f"{base}/token", {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "client_id": client_id,
            "device_code": grant["device_code"],
        })
        err = body.get("error")
        if status == 200 and body.get("access_token"):
            break
        if err == "authorization_pending" or status in (0, 429) or status >= 500:
            continue
        if err == "slow_down":
            interval = min(interval + 5, 60)
            continue
        hint = {
            "expired_token": "the device code expired; run again",
            "authorization_declined": "the sign-in was declined",
            "access_denied": "the sign-in was declined",
        }.get(err, "")
        codes = body.get("error_codes") or []
        if 65001 in codes:
            hint = "consent missing: grant admin consent for the app's delegated permissions"
        elif 7000218 in codes:
            hint = "enable 'Allow public client flows' on the app registration"
        raise LoginError(f"sign-in failed: HTTP {status} ({describe(body)}){': ' + hint if hint else ''}")

    refresh = body.get("refresh_token")
    if not refresh:
        raise LoginError("no refresh token was issued -- is offline_access consented for the app?")
    if not isinstance(refresh, str) or not TOKEN.fullmatch(refresh):
        raise LoginError("the refresh token has an unexpected shape; refusing to write it to a sourced file")
    return {
        "SHAREPOINT_TENANT_ID": tenant,
        "SHAREPOINT_DELEGATED_CLIENT_ID": client_id,
        "SHAREPOINT_USER_REFRESH_TOKEN": refresh,
    }


def main() -> None:
    out = Path(os.environ.get("SHAREPOINT_DELEGATED_ENV_FILE", REPO_ROOT / ".env.sharepoint.delegated"))
    try:
        values = login(dict(os.environ))
        write_env(out, values)
    except LoginError as e:
        print(f"delegated_login: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"Signed in. Refresh token written to {out} (mode 0600).", file=sys.stderr)


if __name__ == "__main__":
    main()
