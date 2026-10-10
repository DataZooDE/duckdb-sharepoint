"""Two secrets, two drives, chosen by SCOPE (longest prefix). The second
secret names a SITE_ID, so its drive is resolved through Graph. A move across
the two must be refused rather than silently landing in the source drive.
Needs SHAREPOINT_SITE_ID (a site whose default library is NOT the DRIVE_ID
library); skips otherwise."""

from __future__ import annotations

import os
import uuid

import pytest

pytestmark = pytest.mark.live


@pytest.fixture
def site_scratch(graph):
    site = os.environ.get("SHAREPOINT_SITE_ID")
    if not site:
        pytest.skip("SHAREPOINT_SITE_ID not set")
    r = graph._req("GET", f"https://graph.microsoft.com/v1.0/sites/{site}/drive?$select=id")
    r.raise_for_status()
    site_drive = r.json()["id"]
    if site_drive == graph.drive_id:
        pytest.skip("SHAREPOINT_SITE_ID's default library is the DRIVE_ID library")
    other = type(graph)(graph.tenant, graph.client_id, graph.client_secret, site_drive)
    folder = f"_duckdb_sharepoint_e2e_site/run-{uuid.uuid4().hex[:12]}"
    if other.stat("_duckdb_sharepoint_e2e_site") is None:
        other.mkdir("_duckdb_sharepoint_e2e_site")
    other.mkdir(folder)
    try:
        yield site, other, folder
    finally:
        other.delete(folder, permanent=True)


def test_scope_selects_the_drive_and_cross_drive_moves_are_refused(graph, scratch, site_scratch, sql):
    site, other, folder = site_scratch
    secrets = (
        f"CREATE SECRET a (TYPE sharepoint, TENANT_ID '{graph.tenant}', CLIENT_ID '{graph.client_id}', "
        f"CLIENT_SECRET '{graph.client_secret}', DRIVE_ID '{graph.drive_id}', SCOPE 'sharepoint://{scratch}');"
        f"CREATE SECRET b (TYPE sharepoint, TENANT_ID '{graph.tenant}', CLIENT_ID '{graph.client_id}', "
        f"CLIENT_SECRET '{graph.client_secret}', SITE_ID '{site}', SCOPE 'sharepoint://{folder}');"
    )
    sql(f"{secrets} SELECT write_blob('sharepoint://{scratch}/in_a.txt', 'A'::BLOB);"
        f" SELECT write_blob('sharepoint://{folder}/in_b.txt', 'B'::BLOB);")
    # Each landed in its own drive, per Graph.
    assert graph.stat(f"{scratch}/in_a.txt") is not None
    assert other.stat(f"{folder}/in_b.txt") is not None
    assert graph.stat(f"{folder}/in_b.txt") is None
    out = sql(f"{secrets} SELECT move_file('sharepoint://{scratch}/in_a.txt', 'sharepoint://{folder}/moved.txt');",
              expect_error=True)
    assert "different secrets or drives" in out
    assert graph.stat(f"{scratch}/in_a.txt") is not None
