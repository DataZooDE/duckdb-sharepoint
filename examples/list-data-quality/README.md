# A SharePoint list → PII and data quality with anofox_tabular → findings on each item

A customer list in SharePoint usually has typos in emails, phone numbers that
can't be dialled, VAT numbers with wrong check digits, the odd duplicate, and
personal data in a free-text notes field. `data_quality.sql` finds all of it
with [anofox_tabular](https://github.com/DataZooDE/anofox-tabular) and writes
each customer's problems into an **Issues** column on its own list item. A
masked copy for analysts is written next to it as Parquet.

| step | function |
|---|---|
| read the list | `read_sharepoint_list('Customers')` |
| where is personal data? | `pii_scan_table('customers')` |
| table checks | `null_rate(...)`, `duplicate_count(...)` |
| field checks | `email_is_valid`, `phonenumber_is_valid`, `vat_is_valid`, `pii_is_valid_iban`, `pii_contains` |
| findings onto each item | `COPY findings TO 'Customers' (FORMAT sharepoint_list)` (rows with an `id` update that item) |
| masked copy | `pii_mask_column`, `pii_mask` → `COPY … TO 'sharepoint://Shared/customers_masked.parquet'` |

## Run it

1. Create a list **Customers** with single-line text columns **Email**,
   **Phone**, **Country**, **VAT**, **IBAN**, and multi-line columns **Notes**
   and **Issues** (Title is there already).
2. Make the sample data, 40 made-up customers with planted problems:
   `python3 make_sample_data.py` (a ready-made `customers.csv` is checked in).
3. Fill in the `CREATE SECRET` in `data_quality.sql`, uncomment step 0 once to
   load the customers into the list, and run `duckdb < data_quality.sql`.

With the sample data, 19 of 41 customers get an Issue: four invalid emails,
three phone numbers that aren't valid for their country, two wrong VAT check
digits and two missing VATs, one broken IBAN, a duplicate email, and personal
data in five notes. The masked copy keeps the country and shows
`b***********@*******.com`, `+***-***-6701` and `FR14*******************2606`.

The checks and masking were run on the sample data with the community
anofox_tabular on DuckDB v1.5.6. Phone validation needs the country's number
metadata, and that build has none for AT and BE.
