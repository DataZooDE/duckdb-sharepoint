"""search_sharepoint (#67) against the REAL tenant, app-only (which must find
the tenant's search region itself) and as the delegated test user.

Search indexes with a lag of minutes, so the probe file is created once, kept
(outside the per-run scratch folders), and found by a token in its name. On
the run that creates it the search tests skip; later runs find it."""

from __future__ import annotations

import time

import pytest

from tests.test_delegated import _delegated_secret, _require_delegated

pytestmark = pytest.mark.live

TOKEN = "duckdbsearchprobe7q4k"
FOLDER = "_duckdb_sharepoint_e2e_search"
PROBE = f"{FOLDER}/{TOKEN}.csv"


@pytest.fixture(scope="module")
def probe(graph):
    if graph.stat(PROBE) is None:
        if graph.stat(FOLDER) is None:
            graph.mkdir(FOLDER)
        graph.upload(PROBE, b"n\n1\n2\n3\n")
        pytest.skip("search probe file created this run; the search index needs minutes -- the next run tests it")
    return PROBE


def _search(sql, secret: str, extra: str = "") -> list[str]:
    deadline = time.time() + 120
    while True:
        out = sql(f"""{secret}
            SELECT type, name, path LIKE 'sharepoint://%', modified IS NOT NULL, web_url LIKE 'https://%'
            FROM search_sharepoint('{TOKEN}'{extra}) WHERE name = '{TOKEN}.csv';
        """).splitlines()
        rows = [line for line in out if line.startswith("driveItem|")]
        if rows or time.time() > deadline:
            return rows
        time.sleep(15)


def test_app_only_search_finds_the_file_and_reads_it_back(probe, sql, secret_sql):
    rows = _search(sql, secret_sql)
    assert rows == [f"driveItem|{TOKEN}.csv|true|true|true"], rows
    # The path (by path, or id: when Graph leaves the folder out) reads straight back.
    out = sql(f"""{secret_sql}
        SELECT sum(n) FROM read_csv((SELECT path FROM search_sharepoint('{TOKEN}') WHERE name = '{TOKEN}.csv'));
    """).splitlines()
    assert out[-1] == "6"


def test_explicit_region_and_types(probe, sql, secret_sql):
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM search_sharepoint('{TOKEN}', types := ['site']) WHERE type = 'driveItem';
    """).splitlines()
    assert out[-1] == "0"
    out = sql(f"{secret_sql} SELECT * FROM search_sharepoint('{TOKEN}', region := 'XXX');", expect_error=True)
    assert "HTTP 400" in out


def test_delegated_search_finds_the_file(probe, sql):
    _require_delegated()
    rows = _search(sql, _delegated_secret("sp_search_user"))
    assert rows == [f"driveItem|{TOKEN}.csv|true|true|true"], rows
