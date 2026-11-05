"""read_sharepoint_list_attachments (#97) against the REAL tenant.

Graph v1.0 has no API for list attachments; they come from SharePoint's own
REST API, which refuses app-only tokens from a client secret. So:

- the main (client-secret) app gets a clear refusal that names the fix;
- the certificate app (SHAREPOINT_CERT_CLIENT_ID / SHAREPOINT_CERTIFICATE_PEM)
  reads an attachment it put there itself -- this needs a permission of the
  SharePoint API ("Office 365 SharePoint Online", application
  Sites.ReadWrite.All or Sites.Selected with a write grant) on that app, not
  only Graph's. Skips while the app lacks it, unless
  SHAREPOINT_REQUIRE_ATTACHMENTS=1 (then a failure).

Each test makes its own list (run-unique name) and deletes it."""

from __future__ import annotations

import base64
import json
import os
import subprocess
import time
import uuid

import pytest
import requests

from helpers.graph import GRAPH

pytestmark = pytest.mark.live

CERT_NEEDED = ("SHAREPOINT_CERT_CLIENT_ID", "SHAREPOINT_CERTIFICATE_PEM")
CONTENT = b"attached by the e2e suite"


def _ok(r, what):
    assert r.ok, f"{what}: HTTP {r.status_code} {r.text[:300]}"
    return r.json() if r.content else {}


@pytest.fixture
def scratch_list(graph):
    """A new list with one item: (site id, site webUrl, list id, list name, item id)."""
    lib = _ok(graph._req("GET", f"{GRAPH}/drives/{graph.drive_id}/list?$select=parentReference"), "site of drive")
    site = lib["parentReference"]["siteId"]
    web_url = _ok(graph._req("GET", f"{GRAPH}/sites/{site}?$select=webUrl"), "site")["webUrl"]
    name = f"_duckdb_sharepoint_e2e_attach_{uuid.uuid4().hex[:10]}"
    created = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists",
                             json={"displayName": name, "list": {"template": "genericList"}}), "create list")
    try:
        item = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists/{created['id']}/items",
                              json={"fields": {"Title": "with an attachment"}}), "create item")
        yield site, web_url, created["id"], name, int(item["id"])
    finally:
        graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{created['id']}")


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _cert_token(tmp_path, resource: str) -> str:
    """An app-only token for `resource` from the certificate app: a client
    assertion signed with openssl (no extra Python dependency)."""
    pem = tmp_path / "attach.pem"
    pem.touch(mode=0o600)
    pem.write_text(os.environ["SHAREPOINT_CERTIFICATE_PEM"])
    password = os.environ.get("SHAREPOINT_CERTIFICATE_PASSWORD")
    der = subprocess.run(["openssl", "x509", "-in", str(pem), "-outform", "DER"], check=True,
                         capture_output=True).stdout
    sha1 = subprocess.run(["openssl", "dgst", "-sha1", "-binary"], input=der, check=True,
                          capture_output=True).stdout
    tenant, client = os.environ["SHAREPOINT_TENANT_ID"], os.environ["SHAREPOINT_CERT_CLIENT_ID"]
    endpoint = f"https://login.microsoftonline.com/{tenant}/oauth2/v2.0/token"
    now = int(time.time())
    head = _b64url(json.dumps({"alg": "RS256", "typ": "JWT", "x5t": _b64url(sha1)}).encode())
    body = _b64url(json.dumps({"aud": endpoint, "iss": client, "sub": client, "jti": uuid.uuid4().hex,
                               "nbf": now - 60, "exp": now + 600}).encode())
    sign = ["openssl", "dgst", "-sha256", "-sign", str(pem)]
    env = dict(os.environ)
    if password:
        sign += ["-passin", "env:SHAREPOINT_CERTIFICATE_PASSWORD"]
    sig = subprocess.run(sign, input=f"{head}.{body}".encode(), check=True, capture_output=True, env=env).stdout
    r = requests.post(endpoint, timeout=60, data={
        "client_id": client, "grant_type": "client_credentials", "scope": f"{resource}/.default",
        "client_assertion_type": "urn:ietf:params:oauth:client-assertion-type:jwt-bearer",
        "client_assertion": f"{head}.{body}.{_b64url(sig)}"})
    assert r.ok, f"certificate token: HTTP {r.status_code} {r.text[:300]}"
    return r.json()["access_token"]


def _cert_secret(tmp_path) -> str:
    pem = tmp_path / "app.pem"
    pem.touch(mode=0o600)
    pem.write_text(os.environ["SHAREPOINT_CERTIFICATE_PEM"])
    password = ", CERTIFICATE_PASSWORD getenv('SHAREPOINT_CERTIFICATE_PASSWORD')" \
        if os.environ.get("SHAREPOINT_CERTIFICATE_PASSWORD") else ""
    return ("CREATE OR REPLACE SECRET sp_cert (TYPE sharepoint, PROVIDER client_certificate, "
            "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CERT_CLIENT_ID'), "
            f"CERTIFICATE_PATH '{pem}'{password}, DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")


def test_a_client_secret_is_refused_with_the_fix(sql, secret_sql, scratch_list):
    _, _, _, name, _ = scratch_list
    out = sql(f"{secret_sql} SELECT * FROM read_sharepoint_list_attachments('{name}');", expect_error=True)
    # What SharePoint said is part of the message; print it for the record (hld §7a).
    print(out)
    assert "does not accept app-only tokens from a client secret" in out, out
    assert "client_certificate" in out, out


def test_certificate_app_reads_an_attachment(sql, scratch_list, tmp_path):
    missing = [n for n in CERT_NEEDED if not os.environ.get(n)]
    required = os.environ.get("SHAREPOINT_REQUIRE_ATTACHMENTS") == "1"
    if missing:
        if required:
            pytest.fail(f"attachments required, but {', '.join(missing)} not set")
        pytest.skip(f"no certificate app configured -- set {', '.join(CERT_NEEDED)}")
    _, web_url, list_id, name, item = scratch_list
    host = "https://" + web_url.split("/")[2]
    site_path = "/" + "/".join(web_url.split("/")[3:])
    token = _cert_token(tmp_path, host)
    r = requests.post(
        f"{host}{site_path.rstrip('/')}/_api/web/lists(guid'{list_id}')/items({item})"
        "/AttachmentFiles/add(FileName='hello world.txt')",
        headers={"Authorization": f"Bearer {token}", "Accept": "application/json;odata=nometadata"},
        data=CONTENT, timeout=60)
    if r.status_code in (401, 403) and not required:
        pytest.skip(f"the certificate app has no SharePoint API permission yet (HTTP {r.status_code}: "
                    f"{r.text[:200]}) -- add 'Office 365 SharePoint Online' Sites.ReadWrite.All (application)")
    assert r.ok, f"add attachment: HTTP {r.status_code} {r.text[:300]}"

    secret = _cert_secret(tmp_path)
    out = sql(f"""{secret}
        SELECT item_id, file_name, url LIKE '%/hello%20world.txt', octet_length(content), content::VARCHAR
        FROM read_sharepoint_list_attachments('{name}');
        SELECT count(*), any_value(file_name) FROM read_sharepoint_list_attachments('{name}', item_id := {item});
    """).splitlines()
    assert out[-2:] == [f"{item}|hello world.txt|true|{len(CONTENT)}|{CONTENT.decode()}", "1|hello world.txt"], out
    missing_item = sql(f"{secret} SELECT * FROM read_sharepoint_list_attachments('{name}', item_id := {item + 1000});",
                       expect_error=True)
    assert "no such list, item or attachment" in missing_item, missing_item
