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
        SHAREPOINT_TENANT_ID=...
        SHAREPOINT_DELEGATED_CLIENT_ID=...
        SHAREPOINT_USER_REFRESH_TOKEN=...

Load it in the test shell with `set -a; source .env.sharepoint.delegated; set +a`.
The Entra app needs "Allow public client flows" enabled and delegated
Files.ReadWrite.All, Sites.ReadWrite.All and offline_access consented.

Optional: SHAREPOINT_LOGIN_ENDPOINT / SHAREPOINT_GRAPH_ENDPOINT for national
clouds, SHAREPOINT_DELEGATED_ENV_FILE to write elsewhere.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
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


def die(msg: str) -> None:
    print(f"delegated_login: {msg}", file=sys.stderr)
    sys.exit(1)


def https_base(url: str, allowed: set[str], what: str) -> str:
    parsed = urllib.parse.urlsplit(url.rstrip("/"))
    if parsed.scheme != "https" or parsed.hostname not in allowed or parsed.path or parsed.port:
        die(f"{what} must be https://<one of {sorted(allowed)}>")
    return f"https://{parsed.hostname}"


def post(url: str, form: dict[str, str]) -> tuple[int, dict]:
    req = urllib.request.Request(url, data=urllib.parse.urlencode(form).encode(),
                                 headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def describe(body: dict) -> str:
    """The short error and AADSTS code -- never error_description."""
    codes = body.get("error_codes") or []
    return f"{body.get('error', 'error')}" + (f" AADSTS{codes[0]}" if codes else "")


def main() -> None:
    tenant = os.environ.get("SHAREPOINT_TENANT_ID", "")
    client_id = os.environ.get("SHAREPOINT_DELEGATED_CLIENT_ID", "")
    if not tenant or not client_id:
        die("set SHAREPOINT_TENANT_ID and SHAREPOINT_DELEGATED_CLIENT_ID")
    if not all(c.isalnum() or c in "-." for c in tenant):
        die("SHAREPOINT_TENANT_ID must be a tenant GUID or domain")
    login = https_base(os.environ.get("SHAREPOINT_LOGIN_ENDPOINT", "https://login.microsoftonline.com"),
                       ALLOWED_LOGIN_HOSTS, "SHAREPOINT_LOGIN_ENDPOINT")
    graph = https_base(os.environ.get("SHAREPOINT_GRAPH_ENDPOINT", "https://graph.microsoft.com"),
                       ALLOWED_GRAPH_HOSTS, "SHAREPOINT_GRAPH_ENDPOINT")
    scope = f"{graph}/Files.ReadWrite.All {graph}/Sites.ReadWrite.All offline_access"
    out = Path(os.environ.get("SHAREPOINT_DELEGATED_ENV_FILE", REPO_ROOT / ".env.sharepoint.delegated"))

    base = f"{login}/{tenant}/oauth2/v2.0"
    status, grant = post(f"{base}/devicecode", {"client_id": client_id, "scope": scope})
    if status != 200 or "device_code" not in grant:
        die(f"device-code request failed: HTTP {status} ({describe(grant)})")
    if not str(grant.get("verification_uri", "")).startswith("https://"):
        die("device-code response has no https verification_uri")

    # The ONLY thing printed: where to go and what to type.
    print(f"Open {grant['verification_uri']} and enter the code {grant['user_code']}", flush=True)

    interval = max(1, min(int(grant.get("interval") or 5), 60))
    deadline = time.monotonic() + int(grant.get("expires_in") or 900)
    while True:
        time.sleep(interval)
        if time.monotonic() >= deadline:
            die("the device code expired before sign-in completed; run again")
        status, body = post(f"{base}/token", {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "client_id": client_id,
            "device_code": grant["device_code"],
        })
        err = body.get("error")
        if status == 200 and body.get("access_token"):
            break
        if err == "authorization_pending" or status == 429 or status >= 500:
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
        die(f"sign-in failed: HTTP {status} ({describe(body)}){': ' + hint if hint else ''}")

    refresh = body.get("refresh_token")
    if not refresh:
        die("no refresh token was issued -- is offline_access consented for the app?")

    # Create 0600 from the start: never a window where the token is world-readable.
    tmp = out.with_name(out.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("# Written by scripts/delegated_login.py -- git-ignored, never commit.\n")
        f.write(f"SHAREPOINT_TENANT_ID={tenant}\n")
        f.write(f"SHAREPOINT_DELEGATED_CLIENT_ID={client_id}\n")
        f.write(f"SHAREPOINT_USER_REFRESH_TOKEN={refresh}\n")
    os.replace(tmp, out)
    print(f"Signed in. Refresh token written to {out} (mode 0600).", file=sys.stderr)


if __name__ == "__main__":
    main()
