"""ATTACH a SharePoint site (#60) against the REAL tenant: its lists are
tables -- listed, described and read (with filter pushdown), and written with
INSERT, UPDATE and DELETE."""

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
            CREATE TABLE site.nope (a INT);
        """, expect_error=True)
        assert "not supported on an attached SharePoint site" in out
    finally:
        graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{created}")
