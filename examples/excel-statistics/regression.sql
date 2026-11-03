-- Excel in SharePoint -> anofox_statistics -> the model back into the workbook.
--
-- marketing.xlsx (make_sample_workbook.py) holds weekly spend per channel and
-- sales per region in the Excel table "Campaigns". This fits sales on the
-- three channels with ordinary least squares -- with standard errors,
-- confidence intervals and p-values -- and writes the coefficients and the
-- fit into a "Model" sheet of the same workbook, in place.
--
-- Read the result as association, not effect: in real data the channels move
-- together and with the season. See anofox_statistics' docs for robust
-- standard errors (hc_type), regularised and mixed models.

INSTALL sharepoint FROM community;
LOAD sharepoint;
INSTALL anofox_statistics FROM community;
LOAD anofox_statistics;

CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER device_code,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '11111111-1111-1111-1111-111111111111',
    DRIVE_ID  'b!AbCdEf...'
);

-- 1. The data, live from the workbook's table.
CREATE OR REPLACE TABLE campaigns AS
SELECT * FROM read_excel_table('Marketing/marketing.xlsx', 'Campaigns');

-- 2. sales ~ tv + online + print, with a level per region (two dummies;
--    North is the baseline).
CREATE OR REPLACE TABLE fit AS
SELECT ols_fit_agg(sales, [tv, online, print, (region = 'South')::DOUBLE, (region = 'West')::DOUBLE],
                   MAP{'compute_inference': 'true', 'confidence_level': '0.95'}) AS m
FROM campaigns;

-- 3. One row per term.
CREATE OR REPLACE TABLE coefficients AS
SELECT '(intercept)' AS term, round(m.intercept, 3) AS estimate, round(m.intercept_std_error, 3) AS std_error,
       round(m.intercept_conf_low, 3) AS ci_low, round(m.intercept_conf_high, 3) AS ci_high,
       m.intercept_p_value AS p_value
FROM fit
UNION ALL
SELECT ['tv', 'online', 'print', 'region South', 'region West'][i], round(m.coefficients[i], 3),
       round(m.std_errors[i], 3), round(m.conf_low[i], 3), round(m.conf_high[i], 3), m.p_values[i]
FROM fit, range(1, 6) t(i);

-- 4. Into the workbook: the coefficients from Model!A1, the fit beside them.
COPY coefficients TO 'Marketing/marketing.xlsx' (FORMAT excel_range, RANGE 'Model!A1');
COPY (SELECT m.n_observations AS observations, round(m.r_squared, 4) AS r_squared,
             round(m.adj_r_squared, 4) AS adj_r_squared, round(m.residual_std_error, 2) AS residual_se,
             round(m.aic, 1) AS aic
      FROM fit)
TO 'Marketing/marketing.xlsx' (FORMAT excel_range, RANGE 'Model!H1');

FROM coefficients;
