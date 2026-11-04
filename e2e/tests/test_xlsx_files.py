"""Plain .xlsx files through the sharepoint:// filesystem (#96): DuckDB's own
excel extension reads them with read_xlsx (ranged reads; Excel for the web is
not involved) and writes a NEW workbook with COPY ... (FORMAT xlsx), which the
workbook API (read_excel_range) can then read too. The xlsx writer only
appends (checked locally: no backward write, no seek), so it streams through
our upload session like any other COPY. Needs the excel extension (INSTALL
from the core repository)."""

from __future__ import annotations

import pytest

from conftest import REPO_ROOT

pytestmark = pytest.mark.live

FIXTURE = REPO_ROOT / "test" / "data" / "excel" / "fixture.xlsx"


def test_read_xlsx_reads_a_workbook_file(scratch, sql, secret_sql):
    path = f"sharepoint://{scratch}/fixture.xlsx"
    out = sql(f"""{secret_sql}
        INSTALL excel; LOAD excel;
        SELECT write_blob('{path}', content) FROM read_blob('{FIXTURE}');
        SELECT count(*), sum(Units), min(Month), typeof(min(Month)) FROM read_xlsx('{path}', sheet := 'Sales');
        SELECT x FROM read_xlsx('{path}', sheet := 'My Sheet');
    """).splitlines()
    assert out[-2:] == ["3|29.0|2025-01-01|DATE", "1.0"]


def test_copy_format_xlsx_creates_and_overwrites_a_workbook(scratch, sql, secret_sql):
    path = f"sharepoint://{scratch}/report.xlsx"
    out = sql(f"""{secret_sql}
        INSTALL excel; LOAD excel;
        COPY (SELECT i AS n, 'x' || i AS s FROM range(20000) t(i)) TO '{path}' (FORMAT xlsx, HEADER true, SHEET 'Report');
        SELECT count(*), sum(n) FROM read_xlsx('{path}', sheet := 'Report');
    """).splitlines()
    assert out[-1] == f"20000|{float(sum(range(20000)))}"

    # The same workbook through Excel for the web: the file COPY wrote is a
    # real workbook to Graph, so the live API reads it as well.
    out = sql(f"""{secret_sql}
        SELECT n, s FROM read_excel_range('{scratch}/report.xlsx', 'Report!A1:B3') ORDER BY n;
    """).splitlines()
    assert out[-2:] == ["0|x0", "1|x1"]

    # Over an existing file: with USE_TMP_FILE false (DuckDB's xlsx writer
    # still writes the file after COPY's tmp_ move, so the move would find
    # nothing published -- hld §7a). The smaller result replaces the old one.
    out = sql(f"""{secret_sql}
        INSTALL excel; LOAD excel;
        COPY (SELECT 7 AS n) TO '{path}' (FORMAT xlsx, HEADER true, SHEET 'Report', USE_TMP_FILE false);
        SELECT count(*), max(n) FROM read_xlsx('{path}', sheet := 'Report');
        SELECT string_agg(parse_filename(file), ',' ORDER BY file) FROM glob('sharepoint://{scratch}/*');
    """).splitlines()
    assert out[-2:] == ["1|7.0", "report.xlsx"]
