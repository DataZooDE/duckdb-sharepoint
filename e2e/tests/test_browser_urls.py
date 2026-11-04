"""Start without ids (#95): a browser address, a sharing link and SITE_URL
reach the right file, and sharepoint_sites() / sharepoint_drives() find the
addresses and ids.

A browser URL names its own library: the URL tests use a file in a SECOND
drive (SHAREPOINT_SITE_ID's default library) while the secret names
SHAREPOINT_DRIVE_ID, so reaching it at all proves the URL chose the drive.
Skips the second-drive parts when SHAREPOINT_SITE_ID is not configured."""

from __future__ import annotations

import os
import urllib.parse
import uuid

import pytest

from conftest import REPO_ROOT

pytestmark = pytest.mark.live


def _url(base: str, path: str) -> str:
    return base + "/" + urllib.parse.quote(path.strip("/"), safe="/")


def _creds(extra: str) -> str:
    return ("CREATE OR REPLACE SECRET sp_url (TYPE sharepoint, PROVIDER client_credentials, "
            "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CLIENT_ID'), "
            f"CLIENT_SECRET getenv('SHAREPOINT_CLIENT_SECRET'){extra});")


@pytest.fixture
def other_drive(graph):
    """A scratch folder in the second drive, and that drive's Graph client."""
    site = os.environ.get("SHAREPOINT_SITE_ID")
    if not site:
        pytest.skip("SHAREPOINT_SITE_ID not set")
    drive = graph.get(f"/sites/{site}/drive?$select=id")["id"]
    if drive == graph.drive_id:
        pytest.skip("SHAREPOINT_SITE_ID's default library is the DRIVE_ID library")
    other = type(graph)(graph.tenant, graph.client_id, graph.client_secret, drive)
    folder = f"_duckdb_sharepoint_e2e_site/run-{uuid.uuid4().hex[:12]}"
    if other.stat("_duckdb_sharepoint_e2e_site") is None:
        other.mkdir("_duckdb_sharepoint_e2e_site")
    other.mkdir(folder)
    try:
        yield other, folder
    finally:
        other.delete(folder, permanent=True)


def test_browser_url_reads_writes_and_globs_in_its_own_library(other_drive, sql, secret_sql):
    other, folder = other_drive
    base = other.web_url()
    nums = b"n\n1\n2\n3\n"
    other.upload(f"{folder}/nums one.csv", nums)
    url = _url(base, f"{folder}/nums one.csv")
    out = sql(f"""{secret_sql}
        SELECT sum(n) FROM read_csv('{url}');
        SELECT file_size('{url}');
        COPY (SELECT 10 AS n UNION ALL SELECT 20) TO '{_url(base, folder + "/out.parquet")}';
        SELECT count(*), sum(n) FROM read_parquet('{_url(base, folder + "/*.parquet")}');
        SELECT string_agg(file, ',' ORDER BY file) FROM glob('{_url(base, folder + "/*")}');
    """).splitlines()
    assert out[-4:-1] == ["6", str(len(nums)), "2|30"]
    # Globbing a browser URL gives browser URLs back, under the same library.
    assert out[-1] == f"{_url(base, folder + '/nums one.csv')},{_url(base, folder + '/out.parquet')}"
    # The write landed in the URL's library -- not the secret's.
    assert other.stat(f"{folder}/out.parquet") is not None

    # A redirect-style link (Copy link on an opened file: /:x:/r/<path>) too.
    site = other.site_web_url()
    host = urllib.parse.urlsplit(site).netloc
    path = urllib.parse.urlsplit(url).path
    redirect = f"https://{host}/:x:/r{path}?d=w1&csf=1&web=1"
    out = sql(f"{secret_sql} SELECT sum(n) FROM read_csv('{redirect}');").splitlines()
    assert out[-1] == "6"

    # An earlier version by URL: sharepoint_versions() gives URLs with
    # version=, and reading one gives the old content -- from the URL's drive.
    other.upload(f"{folder}/nums one.csv", b"n\n100\n")
    out = sql(f"""{secret_sql}
        SELECT sum(n) FROM read_csv('{url}');
        SELECT path FROM sharepoint_versions('{url}') WHERE NOT current ORDER BY modified LIMIT 1;
    """).splitlines()
    earlier = out[-1]
    assert out[-2] == "100" and earlier.startswith(url + "?version="), out
    out = sql(f"{secret_sql} SELECT sum(n) FROM read_csv('{earlier}');").splitlines()
    assert out[-1] == "6"


def test_sharing_link_reads_one_file_and_refuses_writes(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/shared.csv", b"n\n4\n5\n")
    link = graph.share_link(f"{scratch}/shared.csv")
    assert "/:" in link, link  # a real sharing link, not the item's address
    out = sql(f"{secret_sql} SELECT sum(n) FROM read_csv('{link}');").splitlines()
    assert out[-1] == "9"
    out = sql(f"{secret_sql} COPY (SELECT 1 AS n) TO '{link}' (FORMAT csv);", expect_error=True)
    assert "sharing link" in out, out


def test_site_url_secret_and_excel_by_url(graph, scratch, sql):
    site = graph.site_web_url()
    library = graph.web_url()
    drive_name = graph.get(f"/drives/{graph.drive_id}?$select=name")["name"]
    graph.upload(f"{scratch}/nums.csv", b"n\n7\n8\n")
    fixture = (REPO_ROOT / "test" / "data" / "excel" / "fixture.xlsx").read_bytes()
    graph.upload(f"{scratch}/fixture.xlsx", fixture)

    # SITE_URL + DRIVE by name, and SITE_URL as the library's own address.
    for location in (f", SITE_URL '{site}', DRIVE '{drive_name}'", f", SITE_URL '{library}'"):
        out = sql(f"""{_creds(location)}
            SELECT sum(n) FROM read_csv('sharepoint://{scratch}/nums.csv');
        """).splitlines()
        assert out[-1] == "15", location

    # The workbook API by browser URL, with a secret that names no drive.
    out = sql(f"""{_creds("")}
        SELECT count(*) FROM read_excel_table('{_url(library, scratch + "/fixture.xlsx")}', 'Sales');
    """).splitlines()
    assert out[-1] == "3"

    out = sql(f"""{_creds(f", SITE_URL '{site}', DRIVE 'no such library'")}
        SELECT * FROM read_csv('sharepoint://{scratch}/nums.csv');
    """, expect_error=True)
    assert "no document library named 'no such library'" in out and drive_name in out, out


def test_discovery_finds_the_site_and_its_libraries(graph, sql):
    site = graph.site_web_url()
    library = graph.web_url()
    # A secret with no drive at all: discovery is how one finds it.
    out = sql(f"""{_creds("")}
        SELECT count(*) FROM sharepoint_sites() WHERE lower(rtrim(web_url, '/')) = lower('{site}');
        SELECT count(*) FROM sharepoint_drives('{site}') WHERE id = getenv('SHAREPOINT_DRIVE_ID')
                                                         AND lower(rtrim(web_url, '/')) = lower('{library}');
    """).splitlines()
    assert out[-2:] == ["1", "1"]
    out = sql(f"{_creds('')} SELECT * FROM read_csv('sharepoint://x.csv');", expect_error=True)
    assert "SITE_URL" in out and "sharepoint_sites()" in out, out
