-- A SharePoint list -> anofox_tabfm -> a prediction on every open item.
--
-- The list "Leads" (Industry, Employees, Visits, Downloads, Source, Outcome
-- 'won'/'lost' or empty while open, and empty Prediction / Score columns; see
-- the README) is read live. A tabular foundation model reads the decided leads
-- as in-context examples -- no training -- and scores the open ones; each open
-- lead gets its Prediction and Score written back onto its item.

INSTALL sharepoint FROM community;
LOAD sharepoint;
INSTALL anofox_tabfm FROM community;
LOAD anofox_tabfm;
INSTALL httpfs;                -- the model weights come over HTTPS, once
LOAD httpfs;

CREATE SECRET sp (
    TYPE sharepoint,
    PROVIDER device_code,
    TENANT_ID '00000000-0000-0000-0000-000000000000',
    CLIENT_ID '11111111-1111-1111-1111-111111111111',
    DRIVE_ID  'b!AbCdEf...'
);

-- Mitra (Apache-2.0, ~300 MB, no licence gate), cached under ~/.cache/anofox-tabfm.
CALL tabfm_download('classification', model := 'mitra');

-- 0. Once: fill the list with the sample leads (rows without an id become new items).
-- COPY (SELECT * REPLACE (nullif(Outcome, '') AS Outcome) FROM read_csv('leads.csv'))
-- TO 'Leads' (FORMAT sharepoint_list);

-- 1. The list, live: decided leads teach, open ones get scored.
CREATE OR REPLACE TABLE leads AS
SELECT id, Industry, Employees, Visits, Downloads, Source, Outcome FROM read_sharepoint_list('Leads');
CREATE OR REPLACE VIEW decided AS
SELECT Industry, Employees, Visits, Downloads, Source, Outcome FROM leads WHERE Outcome IS NOT NULL;
CREATE OR REPLACE VIEW open_leads AS
SELECT id, Industry, Employees, Visits, Downloads, Source FROM leads WHERE Outcome IS NULL ORDER BY id;

-- 2. How good is it here? Score the newest 20% of decided leads from the
--    older 80% and compare with what really happened.
CREATE OR REPLACE TABLE history AS
SELECT row_number() OVER (ORDER BY id) AS n, * EXCLUDE (id) FROM leads WHERE Outcome IS NOT NULL;
CREATE OR REPLACE VIEW history_train AS
SELECT * EXCLUDE (n) FROM history WHERE n <= 0.8 * (SELECT count(*) FROM history);
CREATE OR REPLACE VIEW history_test AS
SELECT * EXCLUDE (n, Outcome) FROM history WHERE n > 0.8 * (SELECT count(*) FROM history) ORDER BY n;
SELECT count(*) AS held_out, round(avg((p.yhat = h.Outcome)::INT), 3) AS accuracy,
       round(avg((h.Outcome = 'won')::INT), 3) AS share_won
FROM (SELECT row_number() OVER () AS k, *
      FROM tabfm_classify('history_train', 'Outcome', test := 'history_test',
                          features := ['Industry', 'Employees', 'Visits', 'Downloads', 'Source'], model := 'mitra')) p
JOIN (SELECT row_number() OVER (ORDER BY n) AS k, * FROM history
      WHERE n > 0.8 * (SELECT count(*) FROM history)) h USING (k)
WHERE (p.Visits, p.Downloads, p.Source) IS NOT DISTINCT FROM (h.Visits, h.Downloads, h.Source)
   OR error('predictions out of step with the held-out leads');

-- 3. Score the open leads. The result carries the feature columns, not the
--    id, so it is matched back by position -- and checked: every pair must
--    agree on all features, or nothing is written.
CREATE OR REPLACE TABLE scored AS
WITH predicted AS (
    SELECT row_number() OVER () AS n, *
    FROM tabfm_classify('decided', 'Outcome', test := 'open_leads',
                        features := ['Industry', 'Employees', 'Visits', 'Downloads', 'Source'], model := 'mitra')
), open_rows AS (
    SELECT row_number() OVER (ORDER BY id) AS n, * FROM open_leads
)
SELECT o.id,
       CASE WHEN (o.Industry, o.Employees, o.Visits, o.Downloads, o.Source)
                 IS NOT DISTINCT FROM (p.Industry, p.Employees, p.Visits, p.Downloads, p.Source)
            THEN p.yhat ELSE error('predictions out of step with the open leads') END AS Prediction,
       round(p.proba['won'], 3) AS Score
FROM open_rows o JOIN predicted p USING (n);

-- 4. Onto each open lead: rows with an id update that item.
COPY scored TO 'Leads' (FORMAT sharepoint_list);

SELECT Prediction, count(*) AS leads, round(avg(Score), 3) AS avg_score FROM scored GROUP BY ALL ORDER BY ALL;
