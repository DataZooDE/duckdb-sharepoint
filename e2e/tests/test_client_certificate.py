"""PROVIDER client_certificate (#71) against the REAL tenant: an app that
signs in with a certificate instead of a client secret reads and writes
files, and neither the key nor its password shows up anywhere.

  SHAREPOINT_CERT_CLIENT_ID      the app registration with the certificate
                                 (may be SHAREPOINT_CLIENT_ID's app)
  SHAREPOINT_CERTIFICATE_PEM     the certificate and its RSA private key, PEM
  SHAREPOINT_CERTIFICATE_PASSWORD  optional: the key's password
Skips without them; SHAREPOINT_REQUIRE_CERTIFICATE=1 makes that a failure."""

from __future__ import annotations

import os
import subprocess

import pytest

pytestmark = pytest.mark.live

NEEDED = ("SHAREPOINT_CERT_CLIENT_ID", "SHAREPOINT_CERTIFICATE_PEM")


@pytest.fixture
def cert_secret(graph, tmp_path) -> str:
    missing = [n for n in NEEDED if not os.environ.get(n)]
    if missing:
        if os.environ.get("SHAREPOINT_REQUIRE_CERTIFICATE") == "1":
            pytest.fail(f"certificate suite required, but {', '.join(missing)} not set")
        pytest.skip(f"no certificate app configured -- set {', '.join(NEEDED)}")
    pem = tmp_path / "app.pem"
    pem.touch(mode=0o600)
    pem.write_text(os.environ["SHAREPOINT_CERTIFICATE_PEM"])
    password = ", CERTIFICATE_PASSWORD getenv('SHAREPOINT_CERTIFICATE_PASSWORD')" \
        if os.environ.get("SHAREPOINT_CERTIFICATE_PASSWORD") else ""
    return ("CREATE OR REPLACE SECRET sp_cert (TYPE sharepoint, PROVIDER client_certificate, "
            "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CERT_CLIENT_ID'), "
            f"CERTIFICATE_PATH '{pem}'{password}, DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")


def _assert_no_key_material(text: str) -> None:
    __tracebackhide__ = True
    if "PRIVATE KEY" in text or "BEGIN CERTIFICATE" in text:
        pytest.fail("key material leaked", pytrace=False)
    password = os.environ.get("SHAREPOINT_CERTIFICATE_PASSWORD")
    if password and password in text:
        pytest.fail("the certificate password leaked", pytrace=False)


def test_certificate_signs_in_and_reads_and_writes(scratch, sql, cert_secret):
    out = sql(f"""{cert_secret}
        COPY (SELECT range AS n FROM range(100)) TO 'sharepoint://{scratch}/cert.parquet';
        SELECT sum(n) FROM 'sharepoint://{scratch}/cert.parquet';
        SELECT secret_string FROM duckdb_secrets() WHERE name = 'sp_cert';
    """)
    assert out.splitlines()[-2] == "4950"
    _assert_no_key_material(out)


def test_wrong_certificate_is_refused_cleanly(scratch, sql, cert_secret, tmp_path):
    # Another key under the same app: Entra refuses it (AADSTS700027).
    other = tmp_path / "other.pem"
    key, crt = tmp_path / "k.pem", tmp_path / "c.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key),
                    "-out", str(crt), "-subj", "/CN=not-registered", "-days", "1"],
                   check=True, capture_output=True)
    other.write_bytes(crt.read_bytes() + key.read_bytes())
    bad = cert_secret.split("CERTIFICATE_PATH '")[0] + f"CERTIFICATE_PATH '{other}', " \
        "DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));"
    out = sql(f"{bad} SELECT * FROM read_blob('sharepoint://{scratch}/x');", expect_error=True)
    assert "AADSTS" in out, out
    _assert_no_key_material(out)
