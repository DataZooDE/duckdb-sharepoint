"""ATTACH a SharePoint site (#60) against the REAL tenant: its lists are
tables -- listed, described and read (with filter pushdown), written with
INSERT, UPDATE and DELETE, and created with CREATE TABLE (AS) (#97)."""

from __future__ import annotations

import uuid

import pytest

from helpers.graph import GRAPH
from tests.test_sharepoint_list import ITEMS, ORDERS, _ok, fixture_lists  # noqa: F401  (a fixture)

pytestmark = pytest.mark.live

ATTACH = "ATTACH 'sharepoint://' AS site (TYPE sharepoint);"


def test_lists_are_tables(fixture_lists, sql, secret_sql):
    out = sql(f"""{secret_sql} {ATTACH}
        SELECT count(*) FROM duckdb_tables() WHERE database_name = 'site' AND table_name = '{ORDERS}';
        SELECT count(*), count(DISTINCT id) FROM site."{ORDERS}";
        SELECT count(*) FROM site."{ORDERS}" WHERE Status = 'open';
        SELECT string_agg(column_name || ' ' || column_type, ', ') FROM (DESCRIBE site."{ORDERS}")
        WHERE column_name IN ('Amount', 'Status', 'Due');
    """).splitlines()
    assert out[-4] == "1"
    assert out[-3] == f"{ITEMS}|{ITEMS}"
    assert out[-2] == str(len([i for i in range(ITEMS) if i % 3 == 0]))
    assert out[-1] == "Amount DOUBLE, Due TIMESTAMP, Status VARCHAR"


def test_insert_update_delete(graph, fixture_lists, sql, secret_sql):
    site, _, _ = fixture_lists
    name = f"_duckdb_sharepoint_e2e_attach_{uuid.uuid4().hex[:8]}"
    created = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists", json={
        "displayName": name, "list": {"template": "genericList"},
        "columns": [{"name": "Amount", "number": {}}]}), "create attach list")["id"]
    try:
        out = sql(f"""{secret_sql} {ATTACH}
            INSERT INTO site."{name}" (Title, Amount) VALUES ('a', 1), ('b', 2), ('c', 3);
            UPDATE site."{name}" SET Amount = Amount * 10 WHERE Title IN ('a', 'c');
            DELETE FROM site."{name}" WHERE Title = 'b';
            SELECT string_agg(Title || '=' || Amount, ',' ORDER BY Title) FROM read_sharepoint_list('{name}');
        """).splitlines()
        assert out[-1] == "a=10.0,c=30.0", out
        out = sql(f"""{secret_sql} {ATTACH}
            INSERT INTO site."{name}" (Title) VALUES ('x') RETURNING id;
        """, expect_error=True)
        assert "RETURNING" in out, out
        out = sql(f"""{secret_sql} {ATTACH}
            DROP TABLE site."{name}";
        """, expect_error=True)
        assert "not supported on an attached SharePoint site" in out
    finally:
        graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{created}")


def _drop_list(graph, site, name):
    lists = _ok(graph._req("GET", f"{GRAPH}/sites/{site}/lists?$select=id,displayName&$top=999"), "lists")["value"]
    for entry in lists:
        if entry.get("displayName") == name:
            graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{entry['id']}")


def test_create_table_as_makes_a_list(graph, fixture_lists, sql, secret_sql):
    """CREATE TABLE site.x AS SELECT ...: a new list, its columns typed from
    the query (Title is the list's own), the rows its items; CREATE TABLE
    (no AS) an empty one; IF NOT EXISTS keeps an existing list."""
    site, _, _ = fixture_lists
    name = f"_duckdb_sharepoint_e2e_ctas_{uuid.uuid4().hex[:8]}"
    empty = f"_duckdb_sharepoint_e2e_ctas_empty_{uuid.uuid4().hex[:8]}"
    try:
        out = sql(f"""{secret_sql} {ATTACH}
            CREATE TABLE site."{name}" AS
            SELECT 'p' || i AS Title, i * 1.5 AS "Unit Price", i AS Qty, i % 2 = 0 AS Even,
                   DATE '2026-01-01' + i AS Day, TIMESTAMP '2026-01-01 12:00:00' + INTERVAL (i) HOUR AS At
            FROM range(25) t(i);
            SELECT count(*), sum(Qty), sum("Unit Price"), count(*) FILTER (WHERE Even), min(Day), max(At)
            FROM read_sharepoint_list('{name}');
            SELECT string_agg(column_name || ' ' || column_type, ', ' ORDER BY column_name)
            FROM (DESCRIBE SELECT Title, "Unit Price", Qty, Even, Day, At FROM read_sharepoint_list('{name}'));
        """).splitlines()
        assert out[-2] == f"25|{float(sum(range(25)))}|{sum(i * 1.5 for i in range(25))}|13|2026-01-01|2026-01-02 12:00:00"
        assert out[-1] == "At TIMESTAMP, Day DATE, Even BOOLEAN, Qty DOUBLE, Title VARCHAR, Unit Price DOUBLE"

        # The new table is usable at once in the same session, and a fresh
        # ATTACH lists it like any other list.
        out = sql(f"""{secret_sql} {ATTACH}
            CREATE TABLE site."{empty}" (Title VARCHAR, Amount DOUBLE);
            INSERT INTO site."{empty}" (Title, Amount) VALUES ('a', 1), ('b', 2);
            CREATE TABLE IF NOT EXISTS site."{empty}" (Other INT);
            SELECT count(*), sum(Amount) FROM site."{empty}";
        """).splitlines()
        assert out[-1] == "2|3.0", out

        out = sql(f"{secret_sql} {ATTACH} CREATE TABLE site.\"{empty}\" (x INT);", expect_error=True)
        assert "already has a list" in out, out
        out = sql(f"{secret_sql} {ATTACH} CREATE TABLE site.bad AS SELECT [1, 2] AS xs;", expect_error=True)
        assert "a list column is text, a number, a boolean, a date or a timestamp" in out, out
    finally:
        _drop_list(graph, site, name)
        _drop_list(graph, site, empty)


def test_workbook_tables(graph, scratch, sql, secret_sql):
    """ATTACH ... (WORKBOOKS '<folder>') (#100): each .xlsx under the folder is
    a schema named by its path, its Excel tables are tables; SELECT reads them
    live, INSERT appends rows."""
    from conftest import REPO_ROOT

    fixture = (REPO_ROOT / "test" / "data" / "excel" / "fixture.xlsx").read_bytes()
    graph.upload(f"{scratch}/fixture.xlsx", fixture)
    wb = f"{scratch}/fixture.xlsx"
    attach = f"ATTACH 'sharepoint://' AS site (TYPE sharepoint, WORKBOOKS '{scratch}');"
    out = sql(f"""{secret_sql} {attach}
        SELECT count(*) FROM duckdb_tables() WHERE database_name = 'site' AND schema_name = '{wb}' AND table_name = 'Sales';
        SELECT count(*), sum(Units) FROM site."{wb}".Sales;
        INSERT INTO site."{wb}".Sales (Region, Units) VALUES ('West', 5);
    """).splitlines()
    assert out[-3:-1] == ["1", "3|29.0"], out
    out = sql(f"""{secret_sql}
        SELECT count(*), sum(Units), count(*) FILTER (WHERE Region = 'West') FROM read_excel_table('{wb}', 'Sales');
    """).splitlines()
    assert out[-1] == "4|34.0|1", out
    out = sql(f"{secret_sql} {attach} DELETE FROM site.\"{wb}\".Sales WHERE Units = 5;", expect_error=True)
    assert "not supported on an attached SharePoint site" in out, out
