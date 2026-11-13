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

import pytest

from conftest import scrub

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
