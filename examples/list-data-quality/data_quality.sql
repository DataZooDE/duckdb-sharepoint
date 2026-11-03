-- A SharePoint list -> anofox_tabular -> findings back onto each item.
--
-- The list "Customers" (Title, Email, Phone, Country, VAT, IBAN, Notes and an
-- empty Issues column; see the README) is read live, scanned for personal
-- data, validated field by field, checked as a table, and every item gets its
-- problems written into its own Issues column. A masked copy for analysts goes
-- to a Parquet file next to it, through sharepoint://.

INSTALL sharepoint FROM community;
LOAD sharepoint;
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;

CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER device_code,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '11111111-1111-1111-1111-111111111111',
    DRIVE_ID  'b!AbCdEf...'
);

-- 0. Once: fill the list with the sample customers (rows without an id are
--    created as new items).
-- COPY (FROM read_csv('customers.csv', all_varchar = true)) TO 'Customers' (FORMAT sharepoint_list);

-- 1. The list, live.
CREATE OR REPLACE TABLE customers AS
SELECT id, Title, Email, Phone, Country, VAT, IBAN, Notes FROM read_sharepoint_list('Customers');

-- 2. Where is personal data? One row per column and kind of PII found.
SELECT column_name, pii_type, match_count, sample_values[1] AS example
FROM pii_scan_table('customers')
WHERE match_count > 0
ORDER BY column_name, pii_type;

-- 3. Table-level checks: pass/fail with a message each.
SELECT 'VAT filled in' AS check_name, status, message FROM null_rate('customers', 'VAT', 0.0)
UNION ALL
SELECT 'one customer per email', status, message FROM duplicate_count('customers', ['Email']);

-- 4. Row-level findings, one text per customer ('' when all is well).
CREATE OR REPLACE TABLE findings AS
WITH dup AS (
    SELECT lower(Email) AS email FROM customers GROUP BY ALL HAVING count(*) > 1
)
SELECT id, concat_ws('; ',
           CASE WHEN NOT email_is_valid(Email, 'regex') THEN 'email is not valid' END,
           CASE WHEN NOT phonenumber_is_valid(Phone, Country) THEN 'phone is not a valid ' || Country || ' number' END,
           CASE WHEN VAT IS NULL THEN 'VAT number missing'
                WHEN NOT vat_is_valid(VAT) THEN 'VAT number fails its check digits' END,
           CASE WHEN NOT pii_is_valid_iban(IBAN) THEN 'IBAN fails its check digits' END,
           CASE WHEN lower(Email) IN (SELECT email FROM dup) THEN 'duplicate email' END,
           CASE WHEN pii_contains(Notes) THEN 'personal data in notes' END
       ) AS Issues
FROM customers;

-- 5. Back onto each item: rows with an id update that item's Issues column.
COPY findings TO 'Customers' (FORMAT sharepoint_list);

-- 6. A masked copy for analysts: no names, partial contact data, notes redacted.
COPY (
    SELECT id, Country,
           pii_mask_column(Email, 'EMAIL', 'partial') AS Email,
           pii_mask_column(Phone, 'PHONE', 'partial') AS Phone,
           pii_mask_column(IBAN, 'IBAN', 'partial') AS IBAN,
           pii_mask(Notes, 'redact') AS Notes
    FROM customers
) TO 'sharepoint://Shared/customers_masked.parquet' (FORMAT parquet);

SELECT count(*) AS customers, count(*) FILTER (WHERE Issues <> '') AS with_issues FROM findings;
