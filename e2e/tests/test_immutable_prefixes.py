"""sharepoint_immutable_prefixes: the documented trade-off, both sides of it.

Under a declared-immutable prefix the metadata of a file is remembered, so an
out-of-band rewrite is NOT noticed -- that is the promise the user makes, and
the README warns about it. Outside the prefix the same rewrite IS noticed.
Pinning both stops the opt-in from silently becoming the default (or the
saving from silently disappearing). Ported from gdrive.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def _size(session, uri):
    return session.query(f"SELECT octet_length(content) FROM read_blob('{uri}')")


def test_rewrite_outside_prefix_is_seen(graph, scratch, session):
    path = f"{scratch}/mutable/a.bin"
    graph.upload(path, b"x" * 10)
    uri = f"sharepoint://{path}"
    session.query(f"SET sharepoint_immutable_prefixes = 'sharepoint://{scratch}/frozen'")
    assert _size(session, uri) == ["10"]
    graph.upload(path, b"x" * 25)
    assert _size(session, uri) == ["25"]


def test_rewrite_inside_prefix_is_not_seen_by_design(graph, scratch, session):
    path = f"{scratch}/frozen/a.bin"
    graph.upload(path, b"x" * 10)
    uri = f"sharepoint://{path}"
    session.query(f"SET sharepoint_immutable_prefixes = 'sharepoint://{scratch}/frozen'")
    assert _size(session, uri) == ["10"]
    session.query("CALL sharepoint_reset_stats()")
    graph.upload(path, b"x" * 25)
    # The remembered metadata (size 10, old cTag) is used: the promise was
    # broken, and the read reflects the old file -- exactly what the README
    # warns about. The open made no stat call for it.
    assert _size(session, uri) == ["10"]
    hits = session.query("SELECT value FROM sharepoint_stats() WHERE metric = 'meta_cache_hits'")
    assert hits == ["1"]
