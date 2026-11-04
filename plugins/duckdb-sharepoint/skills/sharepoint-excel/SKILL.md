---
name: sharepoint-excel
description: >
  Live Excel workbooks in SharePoint / OneDrive with the sharepoint DuckDB
  extension, through the Graph workbook API: read_excel_table (a named table),
  read_excel_range (a range or a sheet's used range, header := false,
  all_varchar := true), and writing back with COPY ... TO (FORMAT excel_table,
  TABLE, SHEET, OVERWRITE) or (FORMAT excel_range, RANGE, HEADER, OVERWRITE);
  type inference from values and number formats, dates, the 1904 date system;
  and plain .xlsx files with DuckDB's excel extension (read_xlsx, COPY ...
  (FORMAT xlsx) for a new workbook). Use when reading an Excel table or range
  from SharePoint into SQL, writing query results (e.g. a forecast) back into
  a workbook, or creating a new .xlsx report.
version: 2026.11.03
user-invocable: false
---

# sharepoint extension — Excel workbooks

```sql
SELECT * FROM read_excel_table('Finance/plan.xlsx', 'Sales');            -- scheme optional
SELECT * FROM read_excel_table('sharepoint://id:01ABC...', 'Sales');
SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input!A1:F200');
SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input', header := false);   -- used range
SELECT * FROM read_excel_table('Finance/plan.xlsx', 'Sales', all_varchar := true);
```

- Types per column from values and number formats: BIGINT, DOUBLE, BOOLEAN, DATE,
  TIMESTAMP, TIME, VARCHAR (mixed with text). Error cells are NULL. Header names are made
  unique (`_2`), empty ones become `column<N>`.
- Workbooks in the 1904 date system (old Mac) are detected and read correctly.
- Read whole at bind; meant for Excel-sized data (millions of cells belong in Parquet).

## Writing back (in place; the workbook keeps its id)
```sql
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast');           -- append rows; creates the table (on a new sheet) if missing
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_table, TABLE 'Forecast', OVERWRITE); -- replace its rows
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_range, RANGE 'Out!B2');              -- header + rows at an anchor
COPY (SELECT ...) TO 'Finance/plan.xlsx' (FORMAT excel_range, RANGE 'Out!B2', HEADER false, OVERWRITE);
```
- Columns of an existing table are matched by name. Dates are written as serials with a
  date format.
- Excel for the web keeps a workbook it opened locked for a while: deleting it right after a
  write can fail with 423.
- The workbook must exist; create one with `FORMAT xlsx` (below) or upload it.

## Workbooks as files (DuckDB's excel extension)
```sql
INSTALL excel; LOAD excel;
SELECT * FROM read_xlsx('sharepoint://Finance/archive/2019.xlsx', sheet := 'Sales');   -- the saved file
COPY (SELECT ...) TO 'sharepoint://Reports/q4.xlsx' (FORMAT xlsx, HEADER true, SHEET 'Summary');  -- new file / replace it
```
Choose: the workbook API (`read_excel_*`, `excel_table` / `excel_range`) for live values and
in-place writes into an existing workbook (other sheets and formulas kept); `read_xlsx` /
`FORMAT xlsx` (to replace an existing file add `USE_TMP_FILE false`) for any .xlsx file Excel for the web will not open, and to create a new
workbook or replace one whole. A `FORMAT xlsx` file is a normal workbook for the API after.
