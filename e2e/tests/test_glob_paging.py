"""A folder with more children than one Graph page (200) is listed completely,
and an external addition is seen by the next glob in the same process."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor

import pytest

pytestmark = pytest.mark.live


@pytest.mark.slow
def test_glob_pages_past_200_children(graph, scratch, sql, secret_sql):
    with ThreadPoolExecutor(8) as pool:
        list(pool.map(lambda i: graph.upload(f"{scratch}/many/f{i:04d}.txt", b"x"), range(230)))
    out = sql(f"{secret_sql} SELECT count(*) FROM glob('sharepoint://{scratch}/many/*.txt');"
              f" SELECT count(*) FROM glob('sharepoint://{scratch}/many/f00[0-4]?.txt');")
    assert out.splitlines()[-2:] == ["230", "50"]


def test_external_addition_is_seen_by_next_glob(graph, scratch, session):
    graph.upload(f"{scratch}/g/a.txt", b"1")
    pattern = f"sharepoint://{scratch}/g/*.txt"
    assert session.query(f"SELECT count(*) FROM glob('{pattern}')") == ["1"]
    graph.upload(f"{scratch}/g/b.txt", b"2")
    assert session.query(f"SELECT count(*) FROM glob('{pattern}')") == ["2"]
