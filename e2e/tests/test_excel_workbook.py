"""read_excel_table / read_excel_range at size, through the Graph workbook API
(#50): a 10,000-row named table with dates, datetimes, numbers, booleans and
empty cells, read by path and by driveItem id. The small typed fixture lives in
test/sql/sharepoint_excel.live.template; this one is about volume, paging into
row windows, and the id: form (the SQL suite cannot learn an item id)."""

from __future__ import annotations

import datetime as dt
import math

import pytest

pytestmark = pytest.mark.live

openpyxl = pytest.importorskip("openpyxl")
from openpyxl.worksheet.table import Table  # noqa: E402

ROWS = 10_000
COLS = 6
CELLS_PER_REQUEST = 20_000  # kCellsPerRequest in src/sharepoint_excel_functions.cpp


def _workbook(path):
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Data"
    ws.append(["id", "day", "stamp", "amount", "flag", "note"])
    start = dt.date(2024, 1, 1)
    for i in range(ROWS):
        note = None if i % 7 == 0 else f"n{i}"  # every 7th note empty
        ws.append([i, start + dt.timedelta(days=i % 366), dt.datetime(2024, 1, 1, 0, 0) + dt.timedelta(minutes=i),
                   i * 0.25, i % 2 == 0, note])
    for row in ws.iter_rows(min_row=2, max_row=ROWS + 1):
        row[1].number_format = "yyyy-mm-dd"
        row[2].number_format = "yyyy-mm-dd hh:mm:ss"
    ws.add_table(Table(displayName="Big", ref=f"A1:F{ROWS + 1}"))
    wb.save(path)


def test_large_table_by_path_and_by_id(graph, scratch, sql, secret_sql, tmp_path):
    local = tmp_path / "big.xlsx"
    _workbook(local)
    path = f"{scratch}/big.xlsx"
    sql(f"{secret_sql} SELECT write_blob('sharepoint://{path}', content) FROM read_blob('{local}');")

    out = sql(f"""{secret_sql}
        CALL sharepoint_reset_stats();
        CREATE TABLE t AS SELECT * FROM read_excel_table('{path}', 'Big');
        SELECT (SELECT value FROM sharepoint_stats() WHERE metric = 'workbook');
        SELECT string_agg(column_type, ',' ORDER BY column_name) FROM (DESCRIBE t);
        SELECT count(*), sum(id), count(note), min(day), max(day), max(stamp), sum(amount),
               count(*) FILTER (WHERE flag) FROM t;
    """).splitlines()
    # Header row + body address, then the rows in windows of <= 20k cells.
    assert int(out[-3]) == 2 + math.ceil(ROWS * COLS / CELLS_PER_REQUEST)
    # Alphabetical: amount, day, flag, id, note, stamp.
    assert out[-2] == "DOUBLE,DATE,BOOLEAN,BIGINT,VARCHAR,TIMESTAMP"
    expected_notes = ROWS - math.ceil(ROWS / 7)
    last_stamp = dt.datetime(2024, 1, 1) + dt.timedelta(minutes=ROWS - 1)
    assert out[-1] == (f"{ROWS}|{ROWS * (ROWS - 1) // 2}|{expected_notes}|2024-01-01|2024-12-31|"
                       f"{last_stamp:%Y-%m-%d %H:%M:%S}|{ROWS * (ROWS - 1) // 2 * 0.25}|{ROWS // 2}")

    # The id: form reads the same workbook; stable across a rename.
    item_id = graph.stat(path)["id"]
    out = sql(f"""{secret_sql}
        SELECT count(*), max(id) FROM read_excel_table('id:{item_id}', 'Big');
        SELECT count(*) FROM read_excel_range('sharepoint://id:{item_id}', 'Data!A1:F101');
    """).splitlines()
    assert out[-2] == f"{ROWS}|{ROWS - 1}"
    assert out[-1] == "100"
