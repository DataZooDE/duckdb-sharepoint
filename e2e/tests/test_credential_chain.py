"""PROVIDER credential_chain (#72) against the REAL tenant. CI has no managed
identity, but it has the Azure CLI: signed in there as the CI app (in a
throwaway AZURE_CONFIG_DIR), the chain falls through the managed identity
probe to `az` and reads and writes files with no credential in the secret.
Skips where az is not installed."""

from __future__ import annotations

import os
import shutil
import subprocess

import pytest

pytestmark = pytest.mark.live

CHAIN = ("CREATE OR REPLACE SECRET sp_chain (TYPE sharepoint, PROVIDER credential_chain, "
         "DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")


@pytest.fixture
def az_signed_in(graph, tmp_path, isolated_home):
    az = shutil.which("az")
    if not az:
        pytest.skip("Azure CLI not installed")
    config = tmp_path / "azure"
    env = dict(os.environ, AZURE_CONFIG_DIR=str(config))
    # The secret over stdin's sibling: argv. az has no stdin form for the
    # secret; the runner is single-tenant and the value is masked in logs.
    proc = subprocess.run([az, "login", "--service-principal", "--username", os.environ["SHAREPOINT_CLIENT_ID"],
                           "--password", os.environ["SHAREPOINT_CLIENT_SECRET"], "--tenant",
                           os.environ["SHAREPOINT_TENANT_ID"], "--allow-no-subscriptions", "--output", "none"],
                          env=env, capture_output=True, text=True, timeout=180)
    if proc.returncode != 0:
        pytest.fail("az login failed (output withheld: it may echo credentials)", pytrace=False)
    isolated_home["AZURE_CONFIG_DIR"] = str(config)
    yield
    subprocess.run([az, "logout"], env=env, capture_output=True, timeout=60)


def test_chain_reads_and_writes_through_az(az_signed_in, scratch, sql):
    out = sql(f"""{CHAIN}
        COPY (SELECT range AS n FROM range(10)) TO 'sharepoint://{scratch}/chain.csv';
        SELECT sum(n) FROM read_csv('sharepoint://{scratch}/chain.csv');
        SELECT secret_string FROM duckdb_secrets() WHERE name = 'sp_chain';
    """).splitlines()
    assert out[-2] == "45"
    assert "client_secret" not in out[-1] and "token" not in out[-1]


def test_chain_without_any_identity_names_every_source(graph, sql, isolated_home, tmp_path):
    isolated_home["AZURE_CONFIG_DIR"] = str(tmp_path / "empty-az")  # az: not signed in
    out = sql(f"{CHAIN} SELECT * FROM read_csv('sharepoint://x.csv');", expect_error=True)
    assert "found no Azure identity" in out and "Azure CLI" in out, out
