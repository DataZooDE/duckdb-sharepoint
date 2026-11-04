---
name: sharepoint-lists
description: >
  SharePoint lists with the sharepoint DuckDB extension: read_sharepoint_list
  (by name or id:, site := another site), typed columns (text, number,
  currency DECIMAL, yes/no, date-time TIMESTAMP UTC, date-only DATE, choice
  VARCHAR or VARCHAR[], lookup value + _id, person name + _email + _id,
  created_by / modified_by), WHERE pushdown to Graph $filter on indexed
  columns (EXPLAIN shows it), paging past 5,000 items, and writing items with
  COPY ... TO '<list>' (FORMAT sharepoint_list): create, update by id, or upsert
  by KEY '<column>' (sync from another system). Use
  when reading a SharePoint list into SQL, filtering a large list, joining
  lists, or creating and updating list items.
version: 2026.11.03
user-invocable: false
---

# sharepoint extension — lists

```sql
SELECT * FROM read_sharepoint_list('Orders');                    -- the secret's site
SELECT * FROM read_sharepoint_list('id:4f2c...');
SELECT * FROM read_sharepoint_list('Orders', site := 'Sales');  -- 'sites/Sales', 'teams/Ops', a Graph site id, 'host:/sites/x'
```

Columns: `id` first; each visible column by its display name (built-in Title stays `Title`);
`created`, `modified`, then `created_by`, `created_by_email`, `created_by_id`, `modified_by…`.
- text → VARCHAR, number → DOUBLE, currency → DECIMAL(18,4), yes/no → BOOLEAN,
  date-time → TIMESTAMP (UTC), date-only → DATE, choice → VARCHAR (multi-select → VARCHAR[]).
- lookup → `<name>` (display value) + `<name>_id`; person → `<name>`, `<name>_email`,
  `<name>_id` (lists for multi-person). Names/emails are looked up only if selected.
- Date-only values are right for sites from UTC-10:00 to UTC+13:30.

## Filters run in SharePoint on indexed columns
`=`, `IN`, ranges/BETWEEN and `LIKE 'abc%'` on **indexed** columns become `$filter`
(`EXPLAIN` shows `Graph $filter`); everything else is filtered by DuckDB after the read, with
identical results. Index a column in *List settings → Indexed columns*. A list with a
multi-select choice column is read whole. Page size: `sharepoint_list_page_size`.

## Writing items
```sql
COPY (SELECT 'A-17' AS Title, 120.0 AS Amount, 2 AS Customer_id) TO 'Orders' (FORMAT sharepoint_list);  -- create
COPY (SELECT id, Amount * 1.1 AS Amount FROM read_sharepoint_list('Orders') WHERE Status = 'open')
TO 'Orders' (FORMAT sharepoint_list);                                                                     -- update by id
```
Columns matched by name; `id` present = update, NULL/absent = create. Lookups and people are
set through `<name>_id`; read-only columns (created, modified, names, emails) are skipped, so a
read result can be written back as it is. Sent in `$batch` calls of 20. `SITE 'Sales'` as a
COPY option for another site.

Upsert by a key column (no `id` needed): `KEY '<column>'` updates the item with that key,
creates one if none has it. Keys are read once per COPY; text matches exactly, numbers by value.
A NULL key, a key two items share, or one repeated in the rows is an error (earlier batches
stay written).
```sql
COPY (SELECT order_no AS OrderNo, status AS Status FROM erp_orders) TO 'Orders' (FORMAT sharepoint_list, KEY 'OrderNo');
```

## Only what changed
```sql
SELECT * FROM sharepoint_list_changes('Orders', token := 'latest');               -- just a token
SELECT * FROM sharepoint_list_changes('Orders', token := (SELECT token FROM sync));
```
The list's columns plus `_change`: `'upsert'`, `'delete'` (only `id` set) or `'checkpoint'`;
the one checkpoint row carries the `token` to store for next time (as `sharepoint_changes`
does for files).

## Planner and To Do tasks
```sql
SELECT bucket, title, status, due, assigned_to FROM read_planner_tasks('<plan id>');
SELECT * FROM read_planner_tasks('Q1 launch', group := '<group id>');                   -- by name
SELECT list, title, status, due FROM read_todo_tasks() WHERE status <> 'completed';     -- as yourself
SELECT * FROM read_todo_tasks(user := 'anna@contoso.com', list := 'Tasks');             -- app-only
```
Needs `Tasks.Read.All` (application) or `Tasks.Read` (delegated: add it to `GRAPH_SCOPE`).

## A site as a database: ATTACH
```sql
ATTACH 'sharepoint://sites/Sales' AS sales (TYPE sharepoint);   -- 'sharepoint://' = the secret's site
SHOW ALL TABLES;                                                 -- the site's lists
SELECT * FROM sales.Orders WHERE Status = 'open';                -- read_sharepoint_list, pushdown included
INSERT INTO sales.Orders (Title, Amount) VALUES ('A-18', 99);
UPDATE sales.Orders SET Status = 'closed' WHERE id = 17;
DELETE FROM sales.Orders WHERE Status = 'cancelled';
```
Visible lists are tables of schema `main`, typed from their column definitions. Lists are read
once per ATTACH: after changing columns in SharePoint, DETACH and ATTACH again.
`CREATE TABLE site.X AS SELECT ...` (or with columns) creates a list: text/number/boolean/DATE
(date-only)/TIMESTAMP columns, `Title` = the list's own; needs Sites.Manage.All. No DROP/ALTER,
no RETURNING.
