#!/usr/bin/env python3
"""Write sales.xlsx: three years of synthetic monthly revenue for three
products, as the Excel table "Sales" (product, month, revenue). Upload it to
SharePoint / OneDrive, e.g. as Finance/sales.xlsx, then run forecast.sql.

    uv run --with openpyxl python make_sample_workbook.py

The numbers are made up: a trend, a yearly season and some noise per product.
"""
import datetime as dt
import math
import random
from pathlib import Path

from openpyxl import Workbook
from openpyxl.worksheet.table import Table, TableStyleInfo

random.seed(51)
OUT = Path(__file__).resolve().parent / "sales.xlsx"
PRODUCTS = {"Widgets": (1200, 15, 0.25), "Gadgets": (800, 25, 0.10), "Gizmos": (400, 5, 0.40)}  # level, trend, season

wb = Workbook()
ws = wb.active
ws.title = "Sales"
ws.append(["product", "month", "revenue"])
for product, (level, trend, season) in PRODUCTS.items():
    for m in range(36):
        month = dt.date(2023 + m // 12, 1 + m % 12, 1)
        revenue = (level + trend * m) * (1 + season * math.sin(2 * math.pi * m / 12)) * random.uniform(0.93, 1.07)
        ws.append([product, month, round(revenue, 2)])
for row in ws.iter_rows(min_row=2):
    row[1].number_format = "yyyy-mm-dd"
    row[2].number_format = "#,##0.00"
table = Table(displayName="Sales", ref=f"A1:C{ws.max_row}")
table.tableStyleInfo = TableStyleInfo(name="TableStyleMedium2", showRowStripes=True)
ws.add_table(table)
ws.column_dimensions["B"].width = 12
ws.column_dimensions["C"].width = 12
wb.save(OUT)
print(OUT)
