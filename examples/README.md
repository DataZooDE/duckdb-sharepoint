# Examples

Each example is one SQL file and a small synthetic data set. Each reads
Microsoft 365 data live, runs one DataZoo extension on it, and writes the
result back where people already look.

| example | Microsoft 365 | extension | written back |
|---|---|---|---|
| [excel-forecast](excel-forecast/) | Excel table of monthly sales | [anofox_forecast](https://github.com/DataZooDE/anofox-forecast): 12 months ahead per product | a `Forecast` sheet in the same workbook |
| [excel-statistics](excel-statistics/) | Excel table of marketing spend | [anofox_statistics](https://github.com/DataZooDE/anofox-statistics): OLS with inference | a `Model` sheet with coefficients, CIs, p-values |
| [list-data-quality](list-data-quality/) | SharePoint list of customers | [anofox_tabular](https://github.com/DataZooDE/anofox-tabular): PII scan, validation, checks | an `Issues` column on each item, a masked Parquet copy |
| [list-lead-scoring](list-lead-scoring/) | SharePoint list of leads | [anofox_tabfm](https://github.com/DataZooDE/anofox-tabfm): zero-shot classification | `Prediction` and `Score` on each open lead |

Each one needs a `sharepoint` secret (see the main README's
[Authentication](../README.md#authentication)), and the workbook or list from
its README. The analysis steps were run on the sample data with the community
builds for DuckDB v1.5.6.
