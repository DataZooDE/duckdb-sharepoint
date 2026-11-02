# A SharePoint list → zero-shot predictions with anofox_tabfm → a score on every open lead

A leads list in SharePoint: industry, company size, web visits, downloads, how
the lead came in, and an Outcome that is `won` or `lost`, or empty while the
lead is open. `lead_scoring.sql` uses
[anofox_tabfm](https://github.com/DataZooDE/anofox-tabfm), a tabular
foundation model. The model reads the decided leads as examples, with **no
training step**, and scores the open ones. Each open lead gets its
**Prediction** and **Score** written onto its own list item.

| step | function |
|---|---|
| the model, once | `tabfm_download('classification', model := 'mitra')` (Apache-2.0, ~300 MB) |
| read the list | `read_sharepoint_list('Leads')` |
| how good is it here? | score the newest 20% of decided leads from the rest |
| score the open leads | `tabfm_classify('decided', 'Outcome', test := 'open_leads', …)` |
| onto each item | `COPY scored TO 'Leads' (FORMAT sharepoint_list)` (rows with an `id` update that item) |

## Run it

1. Create a list **Leads** with columns **Industry** (text), **Employees**,
   **Visits**, **Downloads** (numbers), **Source** (text), **Outcome** (choice:
   won, lost), **Prediction** (text) and **Score** (number).
2. `python3 make_sample_data.py` makes 300 synthetic leads: 240 decided and 60
   open. A ready-made `leads.csv` is checked in.
3. Fill in the `CREATE SECRET` in `lead_scoring.sql`, uncomment step 0 once to
   load the leads, and run `duckdb < lead_scoring.sql`.

On the sample data, the held-out check gets 77% right. Always guessing the
more common outcome gets 73%, so on 240 examples the model adds a little, not
a lot. The check is there so you can see that on your own data before you
trust the scores. Of the 60 open leads, 45 are predicted `won`.

The model output carries the feature columns but not the item id. The script
therefore matches predictions back to leads by position, and checks that every
pair agrees on all features before anything is written.

The scoring was run on the sample data with the community anofox_tabfm
(Mitra) on DuckDB v1.5.6.
