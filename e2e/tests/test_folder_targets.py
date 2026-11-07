"""A write never replaces a folder (exploratory testing of duckdb-gdrive,
2026-11-07, checked here). COPY writes tmp_<name> and moves it onto the name
with conflictBehavior=replace; a direct upload uses replace too. Nothing
looked at what the target name was -- so a COPY onto a folder's name, or a
trailing-slash target (the slash is dropped), went to Graph as a replace.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


def _folder_intact(graph, folder: str) -> None:
    item = graph.stat(folder)
    assert item is not None and "folder" in item, item
    assert graph.stat(f"{folder}/keep.csv") is not None, "the folder's content must survive"


@pytest.mark.parametrize("target,options", [
    ("F", "FORMAT csv"),
    ("F/", "FORMAT csv"),
    ("F", "FORMAT csv, USE_TMP_FILE false"),
])
def test_copy_onto_a_folder_is_refused(graph, scratch, sql, secret_sql, target, options):
    graph.mkdir(f"{scratch}/F")
    graph.upload(f"{scratch}/F/keep.csv", b"n\n1\n")
    out = sql(f"""{secret_sql}
        COPY (SELECT 1 AS n) TO 'sharepoint://{scratch}/{target}' ({options});""", expect_error=True)
    assert "is a folder" in out, out
    _folder_intact(graph, f"{scratch}/F")


def test_move_onto_a_folder_is_refused(graph, scratch, sql, secret_sql):
    graph.mkdir(f"{scratch}/F")
    graph.upload(f"{scratch}/F/keep.csv", b"n\n1\n")
    graph.upload(f"{scratch}/x.csv", b"n\n2\n")
    out = sql(f"""{secret_sql}
        SELECT move_file('sharepoint://{scratch}/x.csv', 'sharepoint://{scratch}/F');""", expect_error=True)
    assert "is a folder" in out, out
    _folder_intact(graph, f"{scratch}/F")
    assert graph.stat(f"{scratch}/x.csv") is not None
