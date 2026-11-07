"""read_sharepoint_files (#65) against the REAL test library: one row per
file matching a glob, with the driveItem's own metadata and the library's
custom columns, typed. The two custom columns are added to the library once
and kept (a library's schema is shared by every run):
  _duckdb_e2e_dept (text) and _duckdb_e2e_amount (number)."""

from __future__ import annotations

import pytest

from helpers.graph import GRAPH
from tests.test_sharepoint_list import _ok

pytestmark = pytest.mark.live

DEPT, AMOUNT = "_duckdb_e2e_dept", "_duckdb_e2e_amount"


@pytest.fixture(scope="module")
def library_columns(graph):
    lib = _ok(graph._req("GET", f"{GRAPH}/drives/{graph.drive_id}/list?$select=id,parentReference"), "library")
    site, list_id = lib["parentReference"]["siteId"], lib["id"]
    have = {c["name"] for c in _ok(graph._req("GET", f"{GRAPH}/sites/{site}/lists/{list_id}/columns"),
                                   "columns")["value"]}
    for name, facet in ((DEPT, {"text": {}}), (AMOUNT, {"number": {}})):
        if name not in have:
            _ok(graph._req("POST", f"{GRAPH}/sites/{site}/lists/{list_id}/columns",
                           json={"name": name, "displayName": name, **facet}), f"add column {name}")
    return site, list_id


def test_files_with_metadata_and_custom_columns(graph, library_columns, scratch, sql, secret_sql):
    for i, (dept, amount) in enumerate([("Finance", 10.5), ("Sales", 20.0), (None, None)]):
        sql(f"{secret_sql} SELECT write_blob('sharepoint://{scratch}/f{i}.csv', 'n\n{i}\n'::BLOB);")
        if dept is not None:
            item = graph.stat(f"{scratch}/f{i}.csv")["id"]
            _ok(graph._req("PATCH", f"{GRAPH}/drives/{graph.drive_id}/items/{item}/listItem/fields",
                           json={DEPT: dept, AMOUNT: amount}), "set fields")
    sql(f"{secret_sql} SELECT write_blob('sharepoint://{scratch}/other.txt', 'x'::BLOB);")

    out = sql(f"""{secret_sql}
        SELECT column_type FROM (DESCRIBE SELECT {DEPT}, {AMOUNT}, size, modified FROM read_sharepoint_files('sharepoint://{scratch}/*.csv'));
        SELECT string_agg(name || ':' || coalesce({DEPT}, '-') || ':' || coalesce({AMOUNT}::VARCHAR, '-'), ',' ORDER BY name),
               bool_and(size = 4), bool_and(path LIKE 'sharepoint://{scratch}/f%.csv'), bool_and(modified IS NOT NULL),
               bool_and(modified_by IS NOT NULL), bool_and(content_type IS NOT NULL), bool_and(web_url LIKE 'https://%'),
               count(DISTINCT id)
        FROM read_sharepoint_files('sharepoint://{scratch}/*.csv');
    """).splitlines()
    assert out[-5:-1] == ["VARCHAR", "DOUBLE", "BIGINT", "TIMESTAMP"]
    assert out[-1] == "f0.csv:Finance:10.5,f1.csv:Sales:20.0,f2.csv:-:-|true|true|true|true|true|true|3"

    # The glob syntax is the filesystem's: alternation, and nothing matched.
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM read_sharepoint_files('sharepoint://{scratch}/{{f0.csv,other.txt}}');
        SELECT count(*) FROM read_sharepoint_files('sharepoint://{scratch}/*.parquet');
    """).splitlines()
    assert out[-2:] == ["2", "0"]


def test_checked_out_by(graph, scratch, sql, secret_sql):
    """A checked-out file says by whom (#98); a file nobody checked out, NULL."""
    from helpers.graph import GRAPH

    graph.upload(f"{scratch}/locked.csv", b"n\n1\n")
    graph.upload(f"{scratch}/free.csv", b"n\n2\n")
    item = graph.stat(f"{scratch}/locked.csv")["id"]
    r = graph._req("POST", f"{GRAPH}/drives/{graph.drive_id}/items/{item}/checkout")
    assert r.status_code in (200, 204), (r.status_code, r.text)
    try:
        out = sql(f"""{secret_sql}
            SELECT string_agg(name || '=' || (checked_out_by IS NOT NULL), ',' ORDER BY name)
            FROM read_sharepoint_files('sharepoint://{scratch}/*.csv');
        """).splitlines()
        assert out[-1] == "free.csv=false,locked.csv=true", out
    finally:
        graph._req("POST", f"{GRAPH}/drives/{graph.drive_id}/items/{item}/discardCheckout")


def test_a_missing_path_is_an_error_not_zero_rows(scratch, sql, secret_sql):
    """A literal path that names nothing: an error, as in sharepoint_permissions
    -- zero rows read as "no such files exist" (found in duckdb-gdrive)."""
    out = sql(f"""{secret_sql}
        SELECT * FROM read_sharepoint_files('sharepoint://{scratch}/missing.csv');""", expect_error=True)
    assert "no such file or folder" in out, out
    # A glob that matches nothing stays zero rows.
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM read_sharepoint_files('sharepoint://{scratch}/*.nothing');""")
    assert out.splitlines()[-1] == "0", out


def test_a_version_path_is_refused(graph, scratch, sql, secret_sql):
    """read_sharepoint_files under a ?version= path reported the CURRENT
    file's metadata (found in duckdb-gdrive, same code shape here)."""
    graph.upload(f"{scratch}/v.csv", b"n\n1\n")
    graph.upload(f"{scratch}/v.csv", b"n\n2\n")
    out = sql(f"""{secret_sql}
        SELECT * FROM read_sharepoint_files('sharepoint://{scratch}/v.csv?version=1.0');""", expect_error=True)
    assert "not one of its versions" in out, out


def test_a_user_mistake_carries_no_report_it_hint(sql, secret_sql):
    """datazoo-banner adds 'Unexpected? Please report it' only to unexpected
    errors since DataZooDE/duckdb-extension-banner#7."""
    out = sql(f"""{secret_sql}
        SELECT * FROM read_sharepoint_files(NULL);""", expect_error=True)
    assert "must not be NULL" in out and "Unexpected?" not in out, out
