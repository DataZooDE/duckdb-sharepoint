"""Least privilege (#70): an app with only the Sites.Selected application
permission, granted the test site, reads and writes files, Excel workbooks and
lists there -- and another site is refused with an error that says how to
grant it.

A second app registration, separate from SHAREPOINT_CLIENT_ID:
  SHAREPOINT_SELECTED_CLIENT_ID / SHAREPOINT_SELECTED_CLIENT_SECRET
      Sites.Selected only, granted `write` on the site of SHAREPOINT_DRIVE_ID
      (README: Least privilege)
  SHAREPOINT_UNGRANTED_SITE
      a site on the same tenant it was NOT granted, as "host:/sites/Name"
Skips without them; SHAREPOINT_REQUIRE_SELECTED=1 makes that a failure."""

from __future__ import annotations

import os

import pytest

from conftest import REPO_ROOT
from tests.test_sharepoint_list import ITEMS, ORDERS, fixture_lists  # noqa: F401  (a fixture)

pytestmark = pytest.mark.live

NEEDED = ("SHAREPOINT_SELECTED_CLIENT_ID", "SHAREPOINT_SELECTED_CLIENT_SECRET", "SHAREPOINT_UNGRANTED_SITE")


@pytest.fixture(scope="module")
def selected_sql(graph) -> str:
    missing = [n for n in NEEDED if not os.environ.get(n)]
    if missing:
        if os.environ.get("SHAREPOINT_REQUIRE_SELECTED") == "1":
            pytest.fail(f"Sites.Selected suite required, but {', '.join(missing)} not set")
        pytest.skip(f"no Sites.Selected app configured -- set {', '.join(NEEDED)}")
    # getenv(), as in conftest's secret_sql: the SQL text never holds a credential.
    return (
        "CREATE OR REPLACE SECRET sp_selected (TYPE sharepoint, PROVIDER client_credentials, "
        "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_SELECTED_CLIENT_ID'), "
        "CLIENT_SECRET getenv('SHAREPOINT_SELECTED_CLIENT_SECRET'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));"
    )


def test_granted_site_files_excel_and_lists(fixture_lists, scratch, sql, selected_sql):
    out = sql(f"""{selected_sql}
        COPY (SELECT range AS n FROM range(10)) TO 'sharepoint://{scratch}/n.csv';
        SELECT sum(n) FROM read_csv('sharepoint://{scratch}/n.csv');
        SELECT count(*) FROM glob('sharepoint://{scratch}/*');
    """).splitlines()
    assert out[-2:] == ["45", "1"]

    fixture = REPO_ROOT / "test" / "data" / "excel" / "fixture.xlsx"
    out = sql(f"""{selected_sql}
        SELECT write_blob('sharepoint://{scratch}/fixture.xlsx', content) FROM read_blob('{fixture}');
        SELECT count(*) FROM read_excel_table('{scratch}/fixture.xlsx', 'Sales');
        SELECT count(*) FROM read_sharepoint_list('{ORDERS}');
    """).splitlines()
    assert out[-2:] == ["3", str(ITEMS)]


def test_ungranted_site_is_refused_with_a_hint(sql, selected_sql):
    site = os.environ["SHAREPOINT_UNGRANTED_SITE"]
    out = sql(f"{selected_sql} SELECT * FROM read_sharepoint_list('Documents', site := '{site}');",
              expect_error=True)
    assert "HTTP 403" in out and "Sites.Selected" in out, out
