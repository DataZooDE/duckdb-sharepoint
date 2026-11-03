"""File versions (#66): sharepoint_versions() lists a file's history, and
"sharepoint://...?version=<label>" reads an earlier version byte for byte --
with read_blob and with a reader that globs (the '?' looks like a glob
character). An earlier version is read-only."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def test_list_and_read_an_earlier_version(scratch, sql, secret_sql):
    path = f"sharepoint://{scratch}/v.csv"
    first = "n\n1\n2\n"
    sql(f"{secret_sql} SELECT write_blob('{path}', '{first}'::BLOB);")
    sql(f"{secret_sql} SELECT write_blob('{path}', 'n\n10\n20\n30\n'::BLOB);")

    out = sql(f"""{secret_sql}
        SELECT count(*), count(*) FILTER (WHERE current), bool_and(path LIKE '{path}%'),
               bool_and(modified IS NOT NULL), bool_and(size > 0)
        FROM sharepoint_versions('{path}');
        SELECT path FROM sharepoint_versions('{path}') WHERE NOT current ORDER BY modified LIMIT 1;
    """).splitlines()
    assert out[-2] == "2|1|true|true|true", out
    earlier = out[-1]
    assert earlier.startswith(f"{path}?version="), earlier

    out = sql(f"""{secret_sql}
        SELECT content::VARCHAR = '{first}' FROM read_blob('{earlier}');
        SELECT sum(n) FROM read_csv('{earlier}');
        SELECT sum(n) FROM read_csv('{path}');
    """).splitlines()
    assert out[-3:] == ["true", "3", "60"]

    out = sql(f"{secret_sql} SELECT write_blob('{earlier}', 'x'::BLOB);", expect_error=True)
    assert "read-only" in out
    out = sql(f"{secret_sql} COPY (SELECT 1) TO '{earlier}';", expect_error=True)
    assert "read-only" in out


def test_unknown_version_is_not_found(scratch, sql, secret_sql):
    path = f"sharepoint://{scratch}/one.csv"
    sql(f"{secret_sql} SELECT write_blob('{path}', 'n\n1\n'::BLOB);")
    out = sql(f"{secret_sql} SELECT * FROM read_blob('{path}?version=999.0');", expect_error=True)
    assert "No files found" in out or "no such file" in out, out
