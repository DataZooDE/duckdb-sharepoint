"""remove_file: true once, false after, and the item is really gone (default:
recycle bin; with sharepoint_permanent_delete: gone for good). Ported from
gdrive's test_delete_semantics.py."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.live


@pytest.mark.parametrize("permanent", [False, True])
def test_remove_file_is_idempotent_and_effective(graph, scratch, sql, secret_sql, permanent):
    path = f"{scratch}/doomed_{permanent}.txt"
    graph.upload(path, b"bye")
    uri = f"sharepoint://{path}"
    out = sql(f"{secret_sql} SET sharepoint_permanent_delete = {str(permanent).lower()};"
              f" SELECT remove_file('{uri}'); SELECT remove_file('{uri}');")
    assert out.splitlines()[-2:] == ["true", "false"]
    assert graph.stat(path) is None


def test_remove_file_on_a_folder_is_an_error_not_false(graph, scratch, sql, secret_sql):
    graph.mkdir(f"{scratch}/sub")
    out = sql(f"{secret_sql} SELECT remove_file('sharepoint://{scratch}/sub');", expect_error=True)
    assert "is a directory" in out
    assert graph.stat(f"{scratch}/sub") is not None


def test_remove_file_on_a_folder_by_id_is_refused(graph, scratch, sql, secret_sql):
    folder = graph.mkdir(f"{scratch}/byid")
    out = sql(f"{secret_sql} SELECT remove_file('sharepoint://id:{folder['id']}');", expect_error=True)
    assert "is a directory" in out
    assert graph.stat(f"{scratch}/byid") is not None
