---
name: sharepoint-excel
description: >
  Live Excel workbooks in SharePoint / OneDrive with the sharepoint DuckDB
  extension, through the Graph workbook API: read_excel_table (a named table),
  read_excel_range (a range or a sheet's used range, header := false,
  all_varchar := true), and writing back with COPY ... TO (FORMAT excel_table,
  TABLE, SHEET, OVERWRITE) or (FORMAT excel_range, RANGE, HEADER, OVERWRITE);
  type inference from values and number formats, dates, the 1904 date system.
  Use when reading an Excel table or range from SharePoint into SQL, or writing
  query results (e.g. a forecast) back into a workbook.
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
- The workbook must exist (create .xlsx files with plain COPY of a file, e.g. DuckDB's
  excel extension, or upload).

## Workbooks in an attached site
`ATTACH 'sharepoint://' AS site (TYPE sharepoint, WORKBOOKS 'Finance/')`: each .xlsx under the
folder is a schema named by its path, its Excel tables are tables (`site."Finance/plan.xlsx".Sales`);
SELECT reads live, INSERT appends (columns by name). No UPDATE/DELETE. Tables are typed by reading
each once per ATTACH.
