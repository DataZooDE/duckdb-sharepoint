"""A PERSISTENT secret survives a restart; a dropped one is gone. Ported from
gdrive. The isolated HOME keeps the developer's ~/.duckdb out of it."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def test_persistent_secret_survives_restart(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/p.txt", b"persist")
    persistent = secret_sql.replace("CREATE OR REPLACE SECRET", "CREATE OR REPLACE PERSISTENT SECRET")
    sql(persistent)
    # A fresh process, no CREATE SECRET: the stored one must apply.
    out = sql(f"SELECT content::VARCHAR FROM read_blob('sharepoint://{scratch}/p.txt');")
    assert out.splitlines()[-1] == "persist"
    sql("DROP PERSISTENT SECRET sp_e2e;")
    out = sql(f"SELECT content FROM read_blob('sharepoint://{scratch}/p.txt');", expect_error=True)
    assert "no `sharepoint` secret configured" in out
