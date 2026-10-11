"""The scenario matrix shared with duckdb-gdrive: sizes across every upload
and block boundary (byte-exact, by checksum), and the formats DuckDB layers on
the filesystem (CSV, JSON, Parquet, hive partitions, union_by_name)."""

from __future__ import annotations

import hashlib
import os

import pytest

pytestmark = pytest.mark.live

CHUNK = 327680  # the 320-KiB upload-session granularity; forced as the chunk size below


@pytest.mark.parametrize("size", [0, 1, CHUNK - 1, CHUNK, CHUNK + 1, 3 * CHUNK + 17])
def test_round_trip_is_byte_exact(graph, scratch, sql, secret_sql, size, tmp_path):
    data = os.urandom(size)
    local = tmp_path / "in.bin"
    local.write_bytes(data)
    uri = f"sharepoint://{scratch}/blob_{size}.bin"
    # Chunk size = 320 KiB so sizes above it take the resumable session (several
    # chunks, verbatim uploadUrl); a small block size so reads cross blocks.
    out = sql(f"{secret_sql} SET sharepoint_upload_chunk_bytes = {CHUNK};"
              f" SET sharepoint_block_size_bytes = 262144;"
              f" SELECT write_blob('{uri}', content) FROM read_blob('{local}');"
              f" SELECT file_size('{uri}'), md5(content) FROM read_blob('{uri}');")
    assert out.splitlines()[-1] == f"{size}|{hashlib.md5(data).hexdigest()}"
    # And what landed in SharePoint is the same bytes, per Graph itself.
    assert graph.stat(f"{scratch}/blob_{size}.bin")["size"] == size


@pytest.mark.slow
def test_large_file_round_trip(graph, scratch, sql, secret_sql, tmp_path):
    size = 64 * 1024 * 1024 + 12345
    data = os.urandom(size)
    local = tmp_path / "big.bin"
    local.write_bytes(data)
    uri = f"sharepoint://{scratch}/big.bin"
    out = sql(f"{secret_sql} SELECT write_blob('{uri}', content) FROM read_blob('{local}');"
              f" SELECT md5(content) FROM read_blob('{uri}');")
    assert out.splitlines()[-1] == hashlib.md5(data).hexdigest()


def test_formats_round_trip(scratch, sql, secret_sql):
    base = f"sharepoint://{scratch}"
    out = sql(f"""{secret_sql}
        INSTALL json; LOAD json;
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.csv' (HEADER);
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.json';
        COPY (SELECT i, 'r' || i AS s FROM range(500) t(i)) TO '{base}/f.parquet';
        SELECT (SELECT sum(i) FROM read_csv('{base}/f.csv')) || '/' ||
               (SELECT sum(i) FROM read_json('{base}/f.json')) || '/' ||
               (SELECT sum(i) FROM read_parquet('{base}/f.parquet'));""")
    assert out.splitlines()[-1] == "124750/124750/124750"


def test_hive_partitioned_write_and_read(scratch, sql, secret_sql):
    base = f"sharepoint://{scratch}/hive"
    out = sql(f"""{secret_sql}
        COPY (SELECT i, i % 3 AS part FROM range(300) t(i)) TO '{base}' (FORMAT parquet, PARTITION_BY (part));
        SELECT count(*) FROM glob('{base}/**/*.parquet');
        SELECT part, count(*), sum(i) FROM read_parquet('{base}/**/*.parquet', hive_partitioning = true)
        GROUP BY part ORDER BY part;""")
    lines = out.splitlines()
    assert lines[-4] == "3"
    assert lines[-3:] == ["0|100|14850", "1|100|14950", "2|100|15050"]


def test_union_by_name_across_files(scratch, sql, secret_sql):
    base = f"sharepoint://{scratch}/ubn"
    out = sql(f"""{secret_sql}
        COPY (SELECT 1 AS a, 2 AS b) TO '{base}/x.parquet';
        COPY (SELECT 3 AS a, 'z' AS c) TO '{base}/y.parquet';
        SELECT count(*), count(b), count(c) FROM read_parquet('{base}/*.parquet', union_by_name = true);""")
    assert out.splitlines()[-1] == "2|1|1"
