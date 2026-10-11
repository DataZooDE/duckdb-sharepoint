"""A change made by ANOTHER client must be visible to a running DuckDB process.

Ported from gdrive's test_stale_cache.py. The extension caches blocks (keyed
by item id + cTag) and, under sharepoint_immutable_prefixes, metadata. Neither
may serve a dead or rewritten file outside that opt-in. Expressing this needs
a warm process, a mutation OUTSIDE it, and a second read from the same
process -- which SQLLogicTest cannot do.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def test_external_delete_and_recreate_is_seen(graph, scratch, session):
    path = f"{scratch}/stale.csv"
    first = graph.upload(path, b"a\n1\n")
    uri = f"sharepoint://{path}"
    assert session.query(f"SELECT count(*) FROM read_csv('{uri}')") == ["1"]

    graph.delete(path, permanent=True)
    second = graph.upload(path, b"a\n1\n2\n")
    assert second["id"] != first["id"], "Graph reused the id; this test would prove nothing"

    assert session.query(f"SELECT count(*) FROM read_csv('{uri}')") == ["2"]


def test_external_overwrite_in_place_is_seen(graph, scratch, session):
    """Same item id, new content: the block cache must key on the cTag."""
    path = f"{scratch}/rewrite.csv"
    first = graph.upload(path, b"a\n1\n")
    uri = f"sharepoint://{path}"
    assert session.query(f"SELECT sum(a) FROM read_csv('{uri}')") == ["1"]

    second = graph.upload(path, b"a\n41\n")
    assert second["id"] == first["id"], "expected an in-place overwrite (same id)"
    assert second["cTag"] != first["cTag"]

    assert session.query(f"SELECT sum(a) FROM read_csv('{uri}')") == ["41"]
