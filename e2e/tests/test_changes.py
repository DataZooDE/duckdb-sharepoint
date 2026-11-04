"""sharepoint_changes (#62) against the REAL drive: a token from 'latest',
then exactly the changes made since -- files added, changed and deleted --
and a checkpoint row carrying the next token every time."""

from __future__ import annotations

import time

import pytest

pytestmark = pytest.mark.live


def _changes(sql, secret_sql, folder: str, token: str | None, expect=None) -> tuple[list[str], str]:
    """Rows (excluding folders) as 'change|name', and the next token. Delta
    can trail a write by seconds: retried until `expect` is met."""
    arg = "NULL" if token is None else f"'{token}'"
    deadline = time.time() + 90
    while True:
        out = sql(f"""{secret_sql}
            SELECT _change || '|' || coalesce(name, '?') FROM sharepoint_changes('sharepoint://{folder}', token := {arg})
            WHERE _change <> 'checkpoint' AND NOT coalesce(is_folder, false) ORDER BY 1;
            SELECT '#' || count(*) FILTER (WHERE _change = 'checkpoint') || '#' || max(token)
            FROM sharepoint_changes('sharepoint://{folder}', token := {arg});
        """).splitlines()
        rows = [line for line in out if line.startswith(("upsert|", "delete|"))]
        checkpoint = [line for line in out if line.startswith("#")][-1]
        if expect is None or expect(rows) or time.time() > deadline:
            break
        time.sleep(10)
    count, next_token = checkpoint[1:].split("#", 1)
    assert count == "1", checkpoint
    assert next_token, checkpoint
    return rows, next_token


def test_changes_since_a_token(graph, scratch, sql, secret_sql):
    rows, t0 = _changes(sql, secret_sql, scratch, "latest")
    assert rows == []

    sql(f"""{secret_sql}
        SELECT write_blob('sharepoint://{scratch}/a.csv', 'n\n1\n'::BLOB);
        SELECT write_blob('sharepoint://{scratch}/b.csv', 'n\n2\n'::BLOB);
    """)
    rows, t1 = _changes(sql, secret_sql, scratch, t0, lambda r: len(r) >= 2)
    assert rows == ["upsert|a.csv", "upsert|b.csv"], rows

    a_id = graph.stat(f"{scratch}/a.csv")["id"]
    graph.delete(f"{scratch}/a.csv", permanent=True)
    sql(f"{secret_sql} SELECT write_blob('sharepoint://{scratch}/b.csv', 'n\n3\n'::BLOB);")
    rows, _ = _changes(sql, secret_sql, scratch, t1, lambda r: len(r) >= 2)
    # A delete may come without its name or path; it is reported by id.
    assert "upsert|b.csv" in rows and len([r for r in rows if r.startswith("delete|")]) == 1, rows
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM sharepoint_changes('sharepoint://{scratch}', token := '{t1}')
        WHERE _change = 'delete' AND id = '{a_id}';
    """).splitlines()
    assert out[-1] == "1"


def test_without_a_token_lists_everything_under_the_folder(scratch, sql, secret_sql):
    sql(f"{secret_sql} SELECT write_blob('sharepoint://{scratch}/sub/c.csv', 'n\n1\n'::BLOB);")
    rows, _ = _changes(sql, secret_sql, scratch, None, lambda r: "upsert|c.csv" in r)
    assert rows == ["upsert|c.csv"], rows
