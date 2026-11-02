# Excel in SharePoint → anofox_forecast → back into the workbook

One SQL file that reads monthly sales from an Excel table in SharePoint,
forecasts the next 12 months per product with
[anofox_forecast](https://github.com/DataZooDE/anofox-forecast), and writes
the forecast into the same workbook. It is written in place, so the workbook
can stay open in Excel while the query runs.

1. Make the sample workbook (synthetic data: three products, 2023–2025), or
   use your own with a table of `product`, `month` and `revenue`:

   ```bash
   uv run --with openpyxl python make_sample_workbook.py   # writes sales.xlsx
   ```

   A ready-made `sales.xlsx` is checked in next to it.

2. Upload `sales.xlsx` to a document library, e.g. as `Finance/sales.xlsx`.
3. Put your tenant, app and drive ids into the `CREATE SECRET` in
   `forecast.sql`, then run it:

   ```bash
   duckdb < forecast.sql
   ```

The workbook gets a `Forecast` sheet with `product`, `month`, `forecast`,
`low`, `high` (a 90% prediction interval) and `model`. Running it again
overwrites the same cells.

| step | function |
|---|---|
| read the Excel table | `read_excel_table('Finance/sales.xlsx', 'Sales')` |
| forecast per product | `ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')` |
| write it back | `COPY forecast TO 'Finance/sales.xlsx' (FORMAT excel_range, RANGE 'Forecast!A1')` |
