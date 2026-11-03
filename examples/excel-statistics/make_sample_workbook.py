#!/usr/bin/env python3
"""Write marketing.xlsx: two years of synthetic weekly spend per channel and
sales for three regions, as the Excel table "Campaigns" (week, region, tv,
online, print, sales). Upload it to SharePoint / OneDrive, e.g. as
Marketing/marketing.xlsx, then run regression.sql.

    uv run --with openpyxl python make_sample_workbook.py

Sales are made up: 0.8 per unit of TV spend, 2.1 per unit online, nearly
nothing for print, a level per region, and noise -- so the fit has a known
answer to come back to.
"""
import datetime as dt
import random
from pathlib import Path

from openpyxl import Workbook
from openpyxl.worksheet.table import Table, TableStyleInfo

random.seed(52)
OUT = Path(__file__).resolve().parent / "marketing.xlsx"
REGIONS = {"North": 400.0, "South": 320.0, "West": 360.0}

wb = Workbook()
ws = wb.active
ws.title = "Campaigns"
ws.append(["week", "region", "tv", "online", "print", "sales"])
start = dt.date(2024, 1, 1)
for w in range(104):
    for region, level in REGIONS.items():
        tv = round(random.uniform(50, 250), 1)
        online = round(random.uniform(20, 120), 1)
        prnt = round(random.uniform(0, 60), 1)
        sales = level + 0.8 * tv + 2.1 * online + 0.05 * prnt + random.gauss(0, 40)
        ws.append([start + dt.timedelta(weeks=w), region, tv, online, prnt, round(sales, 1)])
for row in ws.iter_rows(min_row=2):
    row[0].number_format = "yyyy-mm-dd"
table = Table(displayName="Campaigns", ref=f"A1:F{ws.max_row}")
table.tableStyleInfo = TableStyleInfo(name="TableStyleMedium9", showRowStripes=True)
ws.add_table(table)
ws.column_dimensions["A"].width = 12
wb.save(OUT)
print(OUT)
