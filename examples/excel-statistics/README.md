# Excel in SharePoint → a regression with anofox_statistics → the model in the workbook

A marketing workbook in SharePoint holds weekly spend on TV, online and print,
and sales, per region. `regression.sql` fits sales on the three channels with
[anofox_statistics](https://github.com/DataZooDE/anofox-statistics). It uses
ordinary least squares with standard errors, 95% confidence intervals and
p-values, and writes the coefficients and the fit into a **Model** sheet of
the same workbook, in place.

| step | function |
|---|---|
| read the Excel table | `read_excel_table('Marketing/marketing.xlsx', 'Campaigns')` |
| fit | `ols_fit_agg(sales, [tv, online, print, …], MAP{'compute_inference': 'true'})` |
| write it back | `COPY coefficients TO 'Marketing/marketing.xlsx' (FORMAT excel_range, RANGE 'Model!A1')` |

## Run it

1. `uv run --with openpyxl python make_sample_workbook.py` writes
   `marketing.xlsx` (two years × three regions, made up; it is also checked in).
2. Upload it as `Marketing/marketing.xlsx`, fill in the `CREATE SECRET` in
   `regression.sql` and run `duckdb < regression.sql`.

The sample data has a known answer: 0.8 per unit of TV, 2.1 per unit online,
nearly nothing for print. The fit recovers it:

| term | estimate | 95% CI | p |
|---|---:|---|---:|
| tv | 0.82 | 0.75 – 0.90 | < 0.001 |
| online | 2.07 | 1.92 – 2.21 | < 0.001 |
| print | −0.04 | −0.29 – 0.22 | 0.77 |

with R² 0.83 on 312 weeks × regions. Read such a table as association, not
effect: in real data the channels move together and with the season.

The model was run on the sample workbook with the community anofox_statistics
on DuckDB v1.5.6.
