"""WHERE pushdown for read_sharepoint_list (#61) against a REAL list past the
5,000-item list view threshold: predicates on indexed columns become Graph's
$filter (fewer pages), a predicate on a non-indexed column stays in DuckDB
(Graph would refuse it past 5,000 items), and every result equals the one
read without pushdown.

The fixture list `_duckdb_sharepoint_e2e_big_v<N>` (5,100 items) is
provisioned once and reused, like test_sharepoint_list's: Status (a
drop-down choice) and Amount are indexed, Note is not. Bump FIXTURE_VERSION
when its content changes."""

from __future__ import annotations

import pytest

from helpers.graph import GRAPH
from tests.test_sharepoint_list import _batch_post, _find_list, _ok, _site_id

pytestmark = pytest.mark.live

FIXTURE_VERSION = "1"
BIG = f"_duckdb_sharepoint_e2e_big_v{FIXTURE_VERSION}"
ITEMS = 5_100
STATUSES = ["open", "shipped", "closed"]


def _status(i: int) -> str:
    return "rare" if i % 170 == 0 else STATUSES[i % 3]


@pytest.fixture(scope="module")
def big_list(graph):
    """Provisioned under a temporary name and renamed when complete: a list
    found under BIG is whole; a leftover from an interrupted run is rebuilt."""
    site = _site_id(graph)
    found = _find_list(graph, site, BIG)
    if found:
        return site, found
    building = BIG + "_building"
    stale = _find_list(graph, site, building)
    if stale:
        graph._req("DELETE", f"{GRAPH}/sites/{site}/lists/{stale}")
    created = _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists", json={
        "displayName": building, "list": {"template": "genericList"},
        "columns": [
            {"name": "Status", "indexed": True,
             "choice": {"choices": STATUSES + ["rare"], "displayAs": "dropDownMenu"}},
            {"name": "Amount", "indexed": True, "number": {}},
            {"name": "Note", "text": {}},
        ]}), "create big list")["id"]
    _batch_post(graph, site, created, [{"Title": f"item {i}", "Status": _status(i), "Amount": i, "Note": f"n{i % 10}"}
                                       for i in range(ITEMS)])
    _ok(graph._req("PATCH", f"{GRAPH}/sites/{site}/lists/{created}", json={"displayName": BIG}), "rename big list")
    return site, created


def _pages_and_result(sql, secret_sql, query: str) -> tuple[int, str]:
    out = sql(f"""{secret_sql}
        CALL sharepoint_reset_stats();
        CREATE TABLE r AS {query};
        SELECT (SELECT value FROM sharepoint_stats() WHERE metric = 'list');
        SELECT count(*), coalesce(sum(Amount), 0), coalesce(string_agg(DISTINCT Status, ',' ORDER BY Status), '') FROM r;
    """).splitlines()
    return int(out[-2]), out[-1]


def _expected(keep) -> str:
    rows = [i for i in range(ITEMS) if keep(i)]
    statuses = sorted({_status(i) for i in rows})
    return f"{len(rows)}|{float(sum(rows))}|{','.join(statuses)}"


SRC = f"read_sharepoint_list('{BIG}')"


@pytest.mark.parametrize("where, keep", [
    ("Status = 'rare'", lambda i: _status(i) == "rare"),
    ("Status IN ('rare', 'open') AND Amount >= 5000", lambda i: _status(i) in ("rare", "open") and i >= 5000),
    ("Amount > 4990 AND Amount < 5003", lambda i: 4990 < i < 5003),
    ("Status LIKE 'ra%'", lambda i: _status(i) == "rare"),
    ("5050 <= Amount", lambda i: i >= 5050),
])
def test_indexed_predicates_are_pushed_down(big_list, sql, secret_sql, where, keep):
    pages, result = _pages_and_result(sql, secret_sql, f"SELECT * FROM {SRC} WHERE {where}")
    assert result == _expected(keep)
    # drive -> site, list, columns, one page, one $batch for the people's
    # names: not the 6 pages of a full read.
    assert pages <= 5, f"{pages} list calls -- the filter was not pushed down"


def test_unpushed_predicates_give_the_same_rows(big_list, sql, secret_sql):
    """A predicate on a non-indexed column, or on an expression, is not sent to
    Graph (past 5,000 items Graph would refuse it): the whole list is read and
    DuckDB filters. Same rows as the pushed form."""
    pages, result = _pages_and_result(sql, secret_sql, f"SELECT * FROM {SRC} WHERE Note = 'n0' AND Status = 'rare'")
    assert result == _expected(lambda i: i % 10 == 0 and _status(i) == "rare")
    assert pages <= 5  # Status is still pushed; Note is filtered locally

    pages, result = _pages_and_result(sql, secret_sql, f"SELECT * FROM {SRC} WHERE lower(Status) = 'rare'")
    assert result == _expected(lambda i: _status(i) == "rare")
    assert pages >= 3 + ITEMS // 999


def test_explain_shows_the_graph_filter(big_list, sql, secret_sql):
    out = sql(f"{secret_sql} EXPLAIN (FORMAT json) SELECT * FROM {SRC} WHERE Status = 'rare' AND Note = 'n0';")
    assert "fields/Status eq 'rare'" in out
    assert "fields/Note" not in out, "the non-indexed Note was pushed"


def test_case_sensitive_sql_on_a_case_insensitive_server(big_list, sql, secret_sql):
    """SharePoint matches 'RARE' to 'rare'; SQL does not. The pushed filter may
    return more, never fewer -- and DuckDB keeps the SQL meaning."""
    _, result = _pages_and_result(sql, secret_sql, f"SELECT * FROM {SRC} WHERE Status = 'RARE'")
    assert result == "0|0.0|"
