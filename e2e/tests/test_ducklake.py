"""DuckLake with its DATA_PATH on SharePoint: create, insert, update, read an
old snapshot, compact. The workload the immutable-prefix and block-cache work
exists for. Needs the ducklake extension (INSTALL from the core repository)."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def test_ducklake_on_sharepoint(scratch, sql, secret_sql, tmp_path):
    catalog = tmp_path / "lake.ducklake"
    data = f"sharepoint://{scratch}/lake/"
    out = sql(f"""{secret_sql}
        INSTALL ducklake; LOAD ducklake;
        SET sharepoint_immutable_prefixes = '{data.rstrip('/')}';
        ATTACH 'ducklake:{catalog}' AS lake (DATA_PATH '{data}');
        CREATE TABLE lake.t AS SELECT i, i * 2 AS v FROM range(1000) t(i);
        INSERT INTO lake.t SELECT i, i * 2 FROM range(1000, 1500) t(i);
        UPDATE lake.t SET v = -1 WHERE i < 10;
        SELECT 'now', count(*), sum(v) FROM lake.t;
        SELECT 'v1', count(*) FROM lake.t AT (VERSION => 1);
        CALL ducklake_merge_adjacent_files('lake');
        SELECT 'compacted', count(*), sum(v) FROM lake.t;""")
    rows = {line.split("|")[0]: line for line in out.splitlines() if "|" in line}
    expected_sum = sum(i * 2 for i in range(10, 1500)) - 10
    assert rows["now"] == f"now|1500|{expected_sum}"
    assert rows["v1"] == "v1|1000"
    assert rows["compacted"] == f"compacted|1500|{expected_sum}"
