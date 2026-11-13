"""A COPY into a workbook that FAILS leaves the workbook alone.

DuckDB v1.5.6 cleans up after a failed COPY by calling
FileSystem::TryRemoveFile on every file the COPY "created" (the copy global
state's destructor). For the in-place writers (FORMAT excel_range /
excel_table, rewritten to USE_TMP_FILE false) that "file" is the target
itself -- the user's workbook. Before the guard the cleanup DID send the
DELETE; the workbook survived only because SharePoint refused it (423
resourceLocked) while the writer's workbook session was still open -- not
once that session has expired, e.g. in a query that fails minutes later.
So the assertion is on the DELETE, not only on the workbook surviving.

A bare target ('Finance/plan.xlsx', meaning sharepoint://Finance/plan.xlsx)
was cleaned up through the LOCAL filesystem: a same-named local file in the
working directory was deleted.

Found in duckdb-gdrive (FORMAT sheet_range trashed the Sheet). 2.0's
copy_output_lifecycle.cpp keeps targets that existed before the COPY.
See CLAUDE.md, 2026-11-13.
"""

from __future__ import annotations

import subprocess
import time

import pytest

from conftest import scrub
from helpers.graph import GRAPH

pytestmark = pytest.mark.live

openpyxl = pytest.importorskip("openpyxl")
from openpyxl.worksheet.table import Table  # noqa: E402

# The error fires well after the first chunk, so the writer is initialised
# before the query fails -- a mid-stream failure, not a bind error.
FAILING_QUERY = "SELECT CASE WHEN i < 3000 THEN i ELSE error('boom') END AS n FROM range(10000) t(i)"


def _upload_workbook(sql, secret_sql, tmp_path, path):
    local = tmp_path / "keep.xlsx"
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Data"
    ws.append(["n"])
    for i in range(3):
        ws.append([i])
    ws.add_table(Table(displayName="T", ref="A1:A4"))
    wb.save(local)
    sql(f"{secret_sql} SELECT write_blob('sharepoint://{path}', content) FROM read_blob('{local}');")


@pytest.mark.parametrize("target_form", ["scheme", "bare", "browser_url", "sharing_link"])
@pytest.mark.parametrize("fmt", ["excel_range, RANGE 'Data!C1'", "excel_table, TABLE 'T'"])
def test_a_failed_copy_keeps_the_workbook(graph, scratch, sql, secret_sql, duckdb_cli, isolated_home, tmp_path,
                                          fmt, target_form):
    path = f"{scratch}/keep.xlsx"
    _upload_workbook(sql, secret_sql, tmp_path, path)
    before = graph.stat(path)
    assert before is not None
    if target_form == "sharing_link":
        target = graph.share_link(path)
    else:
        target = {
            "scheme": f"sharepoint://{path}",
            "bare": path,
            "browser_url": f"{graph.web_url()}/{path}",
        }[target_form]

    # Run in a directory holding a LOCAL file at the bare path.
    cwd = tmp_path / "cwd"
    local_twin = cwd / path
    local_twin.parent.mkdir(parents=True)
    local_twin.write_bytes(b"a local file that merely shares the name")

    statements = f"""{secret_sql}
        SET threads = 1;
        CALL sharepoint_reset_stats();
        COPY ({FAILING_QUERY}) TO '{target}' (FORMAT {fmt});
        SELECT 'deletes=' || value FROM sharepoint_stats() WHERE metric = 'delete';
    """
    proc = subprocess.run([str(duckdb_cli), "-noheader", "-list"], input=statements, capture_output=True,
                          text=True, timeout=600, env=isolated_home, cwd=cwd)
    out = scrub(proc.stdout + proc.stderr)
    assert "boom" in out, out
    # The cleanup must not even try: before the fix this was deletes=1, and
    # only the workbook session's lock (423) kept the workbook.
    assert "deletes=0" in out, out

    after = graph.stat(path)
    assert after is not None, f"the failed COPY removed the workbook ({target_form}, {fmt})"
    assert after["id"] == before["id"]
    assert local_twin.exists(), "the failed COPY removed a local file of the same name"
    out = sql(f"{secret_sql} SELECT count(*) FROM read_excel_table('{path}', 'T');")
    assert int(out.splitlines()[-1]) >= 3


@pytest.mark.parametrize("fmt", ["excel_range, RANGE 'Data!C1'", "excel_table, TABLE 'T'"])
def test_a_failed_copy_closes_its_workbook_session(graph, scratch, sql, secret_sql, duckdb_cli, isolated_home,
                                                   tmp_path, fmt):
    """A COPY that fails mid-stream closes its workbook session.

    The writer opens one persistent workbook session for the whole COPY, and
    only finalize used to close it. A failed COPY left it open, and an open
    session keeps the workbook locked: a delete answers 423 resourceLocked for
    minutes (more than 420 s measured), so the user cannot replace or remove
    their own file, and the e2e scratch teardown left folders behind. Without
    the fix this test sees 423 on every attempt.
    """
    path = f"{scratch}/keep.xlsx"
    _upload_workbook(sql, secret_sql, tmp_path, path)
    statements = f"""{secret_sql}
        SET threads = 1;
        COPY ({FAILING_QUERY}) TO 'sharepoint://{path}' (FORMAT {fmt});
    """
    proc = subprocess.run([str(duckdb_cli), "-noheader", "-list"], input=statements, capture_output=True,
                          text=True, timeout=600, env=isolated_home, cwd=tmp_path)
    out = scrub(proc.stdout + proc.stderr)
    assert "boom" in out, out

    item = graph.stat(path)
    assert item is not None, "the failed COPY removed the workbook"
    url = f"{GRAPH}/drives/{graph.drive_id}/items/{item['id']}/permanentDelete"
    # closeSession is answered before the lock is released everywhere, so a
    # short grace period -- far below the minutes an open session holds it.
    statuses = []
    deadline = time.monotonic() + 30
    while True:
        r = graph._req("POST", url)
        statuses.append(r.status_code)
        if r.status_code != 423 or time.monotonic() > deadline:
            break
        time.sleep(3)
    assert r.status_code in (200, 204), f"the workbook stayed locked after the failed COPY: {statuses}"
    assert graph.stat(path) is None
