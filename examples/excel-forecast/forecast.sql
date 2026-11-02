-- Excel in SharePoint -> anofox_forecast -> back into the same workbook.
--
-- sales.xlsx (make_sample_workbook.py) holds monthly revenue per product in
-- the Excel table "Sales". This reads it live, forecasts the next 12 months
-- per product, and writes the forecast into the workbook's "Forecast" sheet --
-- in place, so the workbook can stay open in Excel while it runs.

INSTALL sharepoint FROM community;
LOAD sharepoint;
INSTALL anofox_forecast FROM community;
LOAD anofox_forecast;

-- Sign in as yourself (a browser code); see the README's "Authentication"
-- section for app-only credentials instead.
CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER device_code,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '11111111-1111-1111-1111-111111111111',
    DRIVE_ID  'b!AbCdEf...'
);

-- 1. The history, straight from the workbook's table.
CREATE OR REPLACE TABLE sales AS
SELECT product, month, revenue FROM read_excel_table('Finance/sales.xlsx', 'Sales');

-- 2. Twelve months ahead per product, with a 90% prediction interval.
CREATE OR REPLACE TABLE forecast AS
SELECT product, ds AS month, round(yhat, 2) AS forecast,
       round(yhat_lower, 2) AS low, round(yhat_upper, 2) AS high, model_name AS model
FROM ts_forecast_by('sales', product, month, revenue, 'AutoETS', 12, '1mo')
ORDER BY product, month;

-- 3. Back into the workbook, from Forecast!A1 (the sheet is created if it is
--    missing). Re-running overwrites the same cells. To keep the forecast as
--    an Excel table instead -- which a re-run appends to -- use
--    (FORMAT excel_table, TABLE 'Forecast').
COPY forecast TO 'Finance/sales.xlsx' (FORMAT excel_range, RANGE 'Forecast!A1');

SELECT product, min(month) AS first, max(month) AS last, round(sum(forecast)) AS next_12_months
FROM forecast GROUP BY product ORDER BY product;
