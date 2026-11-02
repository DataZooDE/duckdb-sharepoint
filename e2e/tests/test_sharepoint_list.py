"""read_sharepoint_list against a REAL SharePoint list (#49): typed columns,
a lookup, paging past the 5,000-item list view threshold, by name and by id,
on the secret's site and by an explicit site.

Paging is exercised with a small page (sharepoint_list_page_size) rather
than a 5,000-item list: nextLink paging is the same at any size, and the
5,000-item threshold matters only for filters and sorting, which
read_sharepoint_list does not use. The fixture lists are provisioned once and
then reused: `_duckdb_sharepoint_e2e_customers` and
`_duckdb_sharepoint_e2e_orders` on the site of SHAREPOINT_DRIVE_ID. Bump
FIXTURE_VERSION when their content changes -- an older pair is deleted and
rebuilt. Creating lists needs the app's Sites.Manage.All (or FullControl)."""

from __future__ import annotations

import datetime as dt
import math
import time

import pytest

from helpers.graph import GRAPH

pytestmark = pytest.mark.live

FIXTURE_VERSION = "2"
ORDERS = f"_duckdb_sharepoint_e2e_orders_v{FIXTURE_VERSION}"
CUSTOMERS = f"_duckdb_sharepoint_e2e_customers_v{FIXTURE_VERSION}"
ITEMS = 300
PAGE = 50  # sharepoint_list_page_size: six pages, the same nextLink paging a 5,000+ item list takes
STATUSES = ["open", "shipped", "closed"]


def _ok(r, what):
    if r.status_code == 403:
        pytest.fail(f"{what}: 403 -- creating the fixture lists needs Sites.Manage.All for the app: {r.text[:300]}")
    assert r.ok, f"{what}: HTTP {r.status_code} {r.text[:300]}"
    return r.json() if r.content else {}


def _site_id(graph) -> str:
    lib = _ok(graph._req("GET", f"{GRAPH}/drives/{graph.drive_id}/list?$select=parentReference"), "site of drive")
    return lib["parentReference"]["siteId"]


def _find_list(graph, site, name):
    r = graph._req("GET", f"{GRAPH}/sites/{site}/lists?$select=id,displayName&$filter=displayName eq '{name}'")
    hits = [x for x in _ok(r, "find list").get("value", []) if x["displayName"] == name]
    return hits[0]["id"] if hits else None


def _batch_post(graph, site, list_id, fields_list):
    for start in range(0, len(fields_list), 20):  # $batch takes 20 requests
        reqs = [{"id": str(i), "method": "POST", "url": f"/sites/{site}/lists/{list_id}/items",
                 "headers": {"Content-Type": "application/json"}, "body": {"fields": f}}
                for i, f in enumerate(fields_list[start:start + 20])]
        for attempt in range(8):
            out = _ok(graph._req("POST", f"{GRAPH}/$batch", json={"requests": reqs}), "batch")
            failed = [x for x in out["responses"] if x["status"] >= 300]
            if not failed:
                break
            # Throttled or failed parts only; the rest were created. SharePoint
            # throttles bulk item creation: wait as long as it asks.
            wait = max([float((x.get("headers") or {}).get("Retry-After", 0) or 0) for x in failed] + [2.0 ** attempt])
            time.sleep(min(wait, 60.0))
            reqs = [q for q in reqs if q["id"] in {x["id"] for x in failed}]
        else:
            raise AssertionError(f"batch kept failing: {out['responses'][:2]}")


def _order(i: int) -> dict:
    return {"Title": f"order {i}", "Amount": i * 0.5, "Price": round(i * 1.25, 2), "Done": i % 2 == 0,
            "Due": (dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(hours=i)).strftime(
                "%Y-%m-%dT%H:%M:%SZ"),
            "Status": STATUSES[i % 3], "CustomerLookupId": str(1 + i % 3)}


@pytest.fixture(scope="module")
def fixture_lists(graph):
    site = _site_id(graph)
    orders = _find_list(graph, site, ORDERS)
    customers = _find_list(graph, site, CUSTOMERS)
    if orders and customers:
        return site, orders, customers
    for stale in (orders, customers):
        if stale:
            graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{stale}")
    customers = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists", json={
        "displayName": CUSTOMERS, "list": {"template": "genericList"}}), "create customers")["id"]
    _batch_post(graph, site, customers, [{"Title": n} for n in ("Acme", "Globex", "Initech")])
    orders = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists", json={
        "displayName": ORDERS, "list": {"template": "genericList"},
        "columns": [
            {"name": "Amount", "number": {}},
            {"name": "Price", "currency": {"locale": "en-US"}},
            {"name": "Done", "boolean": {}},
            {"name": "Due", "dateTime": {"format": "dateTime"}},
            {"name": "Status", "choice": {"choices": STATUSES}},
            {"name": "Customer", "lookup": {"listId": customers, "columnName": "Title"}},
        ]}), "create orders")["id"]
    _batch_post(graph, site, orders, [_order(i) for i in range(ITEMS)])
    return site, orders, customers


def test_list_types_paging_and_lookup(fixture_lists, sql, secret_sql):
    site, orders, _ = fixture_lists
    out = sql(f"""{secret_sql}
        SET sharepoint_list_page_size = {PAGE};
        CALL sharepoint_reset_stats();
        CREATE TABLE o AS SELECT * FROM read_sharepoint_list('{ORDERS}');
        SELECT (SELECT value FROM sharepoint_stats() WHERE metric = 'list');
        SELECT string_agg(column_name || ' ' || column_type, ', ') FROM (DESCRIBE o);
        SELECT count(*), sum(Amount), sum(Price), count(*) FILTER (WHERE Done), min(Due), max(Due),
               count(DISTINCT Status), count(DISTINCT id), min(created) <= max(modified) FROM o;
        SELECT Customer, Customer_id, count(*) FROM o GROUP BY ALL ORDER BY Customer_id;
    """).splitlines()
    # From the end: CREATE SECRET and CALL print a "true" line each before these.
    out = out[-6:]
    pages = int(out[0])
    # drive -> site, list, columns, then one call per page
    # (plus possibly one empty last page when ITEMS is a multiple of PAGE)
    assert 3 + math.ceil(ITEMS / PAGE) <= pages <= 4 + math.ceil(ITEMS / PAGE), f"{pages} Graph calls"
    assert out[1] == ("id BIGINT, Title VARCHAR, Amount DOUBLE, Price DECIMAL(18,4), Done BOOLEAN, "
                      "Due TIMESTAMP, Status VARCHAR, Customer VARCHAR, Customer_id BIGINT, "
                      "created TIMESTAMP, modified TIMESTAMP")
    last_due = dt.datetime(2026, 1, 1) + dt.timedelta(hours=ITEMS - 1)
    assert out[2] == (f"{ITEMS}|{sum(i * 0.5 for i in range(ITEMS))}|"
                      f"{sum(round(i * 1.25, 2) for i in range(ITEMS)):.4f}|{ITEMS // 2}|"
                      f"2026-01-01 00:00:00|{last_due:%Y-%m-%d %H:%M:%S}|3|{ITEMS}|true")
    assert out[3:] == [f"Acme|1|{ITEMS // 3}", f"Globex|2|{ITEMS // 3}", f"Initech|3|{ITEMS // 3}"]

    # By list id, and with the site named explicitly.
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM read_sharepoint_list('id:{orders}');
        SELECT count(*) FROM read_sharepoint_list('{ORDERS}', site := '{site}');
    """).splitlines()
    assert out == [str(ITEMS), str(ITEMS)]


def test_missing_list_names_it(sql, secret_sql):
    out = sql(f"{secret_sql} SELECT * FROM read_sharepoint_list('_no_such_list_');", expect_error=True)
    assert "list '_no_such_list_'" in out and "not found" in out


def test_copy_creates_and_updates_items(graph, fixture_lists, sql, secret_sql):
    """COPY ... (FORMAT sharepoint_list): rows without an id create items (in
    $batch calls of 20), rows with one update that item, and a
    read_sharepoint_list result can be written back as it is."""
    site, _, customers = fixture_lists
    import uuid
    name = f"_duckdb_sharepoint_e2e_write_{uuid.uuid4().hex[:8]}"
    created = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists", json={
        "displayName": name, "list": {"template": "genericList"},
        "columns": [
            {"name": "Amount", "number": {}},
            {"name": "Due", "dateTime": {"format": "dateTime"}},
            {"name": "Done", "boolean": {}},
            {"name": "Status", "choice": {"choices": STATUSES}},
            {"name": "Customer", "lookup": {"listId": customers, "columnName": "Title"}},
        ]}), "create write list")["id"]
    try:
        out = sql(f"""{secret_sql}
            COPY (SELECT 'row ' || i AS Title, i * 1.5 AS Amount,
                         TIMESTAMP '2026-03-01 12:00:00' + INTERVAL (i) DAY AS Due, i % 2 = 0 AS Done,
                         ['open', 'shipped', 'closed'][1 + i % 3] AS Status, 1 + i % 3 AS Customer_id
                  FROM range(45) t(i))
            TO '{name}' (FORMAT sharepoint_list);
            SELECT count(*), sum(Amount), min(Due), max(Due), count(*) FILTER (WHERE Done),
                   count(DISTINCT Status), string_agg(DISTINCT Customer, ',' ORDER BY Customer)
            FROM read_sharepoint_list('{name}');
        """).splitlines()
        assert out[-1] == (f"45|{sum(i * 1.5 for i in range(45))}|2026-03-01 12:00:00|2026-04-14 12:00:00|23|3|"
                           "Acme,Globex,Initech")

        # Update by id: double the large amounts; nothing is added.
        out = sql(f"""{secret_sql}
            COPY (SELECT id, Amount * 2 AS Amount FROM read_sharepoint_list('{name}') WHERE Amount >= 30)
            TO '{name}' (FORMAT sharepoint_list);
            SELECT count(*), sum(Amount) FROM read_sharepoint_list('{name}');
        """).splitlines()
        expected = sum(i * 1.5 * (2 if i * 1.5 >= 30 else 1) for i in range(45))
        assert out[-1] == f"45|{expected}"

        # A read result written back as it is (created, modified and the
        # lookup's display value are Graph's): an update, not a copy.
        out = sql(f"""{secret_sql}
            COPY (SELECT * REPLACE ('renamed' AS Title) FROM read_sharepoint_list('{name}') WHERE Title = 'row 0')
            TO '{name}' (FORMAT sharepoint_list);
            SELECT count(*), count(*) FILTER (WHERE Title = 'renamed') FROM read_sharepoint_list('{name}');
        """).splitlines()
        assert out[-1] == "45|1"

        out = sql(f"{secret_sql} COPY (SELECT 1 AS nope) TO '{name}' (FORMAT sharepoint_list);", expect_error=True)
        assert 'has no column "nope"' in out
    finally:
        graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{created}")
