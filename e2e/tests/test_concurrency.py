"""A multi-threaded scan returns exactly what a single-threaded one does, and
the shared block cache's single-flight keeps the request count bounded."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def test_parallel_scan_matches_serial_and_bounds_fetches(scratch, sql, secret_sql):
    base = f"sharepoint://{scratch}/par"
    sql(f"""{secret_sql}
        COPY (SELECT i, i % 7 AS k, md5(i::VARCHAR) AS h FROM range(400000) t(i))
          TO '{base}' (FORMAT parquet, PARTITION_BY (k), ROW_GROUP_SIZE 20000);""")
    q = (f"SELECT count(*), sum(i), count(DISTINCT h) "
         f"FROM read_parquet('{base}/**/*.parquet')")
    serial = sql(f"{secret_sql} SET threads = 1; {q};").splitlines()[-1]
    parallel_out = sql(f"""{secret_sql} SET threads = 16; SET sharepoint_block_size_bytes = 1048576;
        CALL sharepoint_reset_stats(); {q};
        SELECT value FROM sharepoint_stats() WHERE metric = 'read_range';
        SELECT value FROM sharepoint_stats() WHERE metric = 'bytes_read';""").splitlines()
    assert parallel_out[-3] == serial == "400000|79999800000|400000"
    read_ranges, bytes_read = int(parallel_out[-2]), int(parallel_out[-1])
    # Single-flight: each 1-MiB block is fetched at most once, so the bytes
    # fetched cannot exceed the files' total size by more than block rounding.
    total = int(sql(f"{secret_sql} SELECT sum(file_size(file)) FROM glob('{base}/**/*.parquet');"
                    ).splitlines()[-1])
    assert bytes_read <= total, f"fetched {bytes_read} bytes of {total}: blocks fetched twice"
    assert read_ranges >= 7
