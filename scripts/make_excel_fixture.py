#!/usr/bin/env python3
"""Regenerate test/data/excel/fixture.xlsx, the workbook the live SQL suite
reads through the Graph workbook API (read_excel_table / read_excel_range).

    uv run --with openpyxl python scripts/make_excel_fixture.py

Synthetic data only. The expected values in test/sql/sharepoint.live.template
are written against exactly this content -- change both together.
"""
import datetime as dt
from pathlib import Path

from openpyxl import Workbook
from openpyxl.worksheet.table import Table

OUT = Path(__file__).resolve().parent.parent / "test" / "data" / "excel" / "fixture.xlsx"

wb = Workbook()
# Formulas carry no cached value from openpyxl; Excel for the web computes
# them when it opens the workbook.
wb.calculation.fullCalcOnLoad = True

# Sheet "Sales": the named table "Sales", one column per inferred type.
ws = wb.active
ws.title = "Sales"
ws.append(["Region", "Month", "Revenue", "Units", "Active", "Note", "Updated", "Start", "Ratio"])
rows = [
    ("North", dt.date(2025, 1, 1), 1200.5, 10, True, "ok", dt.datetime(2025, 1, 1, 8, 30), dt.time(9, 0), 0.5),
    ("South", dt.date(2025, 2, 1), 980.25, 7, False, None, dt.datetime(2025, 2, 1, 17, 45), dt.time(13, 30), 2),
    ("East", dt.date(2025, 3, 1), None, 12, True, "late", dt.datetime(2025, 3, 1, 0, 0), dt.time(7, 15), "=1/0"),
]
for r in rows:
    ws.append(list(r))
for row in ws.iter_rows(min_row=2, max_row=4):
    row[1].number_format = "yyyy-mm-dd"
    row[6].number_format = "yyyy-mm-dd hh:mm"
    row[7].number_format = "h:mm"
ws.add_table(Table(displayName="Sales", ref="A1:I4"))

# Sheet "Input": a plain range with a title above it -- no table.
ws = wb.create_sheet("Input")
ws["A1"] = "Plan 2026 (synthetic)"
ws.append([])  # row 2 left empty in column A; the range starts at B3
ws["B3"], ws["C3"], ws["D3"] = "product", "qty", "price"
ws["B4"], ws["C4"], ws["D4"] = "widget", 3, 2.5
ws["B5"], ws["C5"], ws["D5"] = "gadget", 4, 10
ws["B6"], ws["C6"], ws["D6"] = "doohickey", None, 1.25

# A sheet name that has to be quoted in an address.
ws = wb.create_sheet("My Sheet")
ws["A1"], ws["A2"] = "x", 1

OUT.parent.mkdir(parents=True, exist_ok=True)
wb.save(OUT)
print(OUT)
