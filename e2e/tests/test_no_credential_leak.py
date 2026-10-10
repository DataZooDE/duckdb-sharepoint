"""No credential ever reaches an error message or a listing. Ported from gdrive.

Failure paths are where secrets leak: an exception that echoes a request, a
URL with its signed query, a token in a debug string. Drive several real
failures and grep everything the user can see for the client secret, any
bearer-token-shaped string, and the download URL's tempauth.
"""

from __future__ import annotations

import re

import pytest

pytestmark = pytest.mark.live

JWT = re.compile(r"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")


def _assert_clean(text: str, secret: str) -> None:
    assert secret not in text, "client secret leaked"
    assert not JWT.search(text), "a bearer token leaked"
    assert "tempauth" not in text.lower(), "a signed download/upload URL leaked"


def test_failures_and_listings_leak_nothing(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/f.txt", b"abc")
    bad_secret = secret_sql.replace(graph.client_secret, "wrong-" + graph.client_secret[:4])
    outputs = [
        sql(f"{bad_secret} SELECT * FROM read_blob('sharepoint://{scratch}/f.txt');", expect_error=True),
        # read_csv, not read_blob: read_blob treats its path as a glob and
        # returns zero rows for no match, which is DuckDB's own semantics.
        sql(f"{secret_sql} SELECT * FROM read_csv('sharepoint://{scratch}/nope.csv');", expect_error=True),
        sql(f"{secret_sql} SELECT write_blob('sharepoint://{scratch}/bad:name.txt', 'x'::BLOB);",
            expect_error=True),
        sql(f"{secret_sql} SELECT * FROM read_csv('sharepoint://id:..');", expect_error=True),
        sql(f"{secret_sql} SELECT * FROM duckdb_secrets();"),
        sql(f"{secret_sql} SELECT content FROM read_blob('sharepoint://{scratch}/f.txt');"),
    ]
    for out in outputs:
        _assert_clean(out, graph.client_secret)
