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
    # Who changed it (#99): the CI app, as an application -- named, no email.
    out = sql(f"""{secret_sql}
        SELECT count(*) FILTER (WHERE modified_by IS NOT NULL), count(*)
        FROM sharepoint_changes('sharepoint://{scratch}', token := '{t0}')
        WHERE _change = 'upsert' AND NOT is_folder;
    """).splitlines()
    assert out[-1] == "2|2", out

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


# The mirror pattern of the README (#99), run as written: a DuckLake table of
# a folder's files kept current from sharepoint_changes, the token stored
# beside it.
MIRROR_ONCE = """
    INSTALL ducklake; LOAD ducklake;
    ATTACH 'ducklake:{catalog}' AS lake (DATA_PATH '{data}');
    -- The token first, then the listing: a change in between shows up twice, not never.
    CREATE TABLE lake.sync AS
        SELECT token FROM sharepoint_changes('sharepoint://{folder}/', token := 'latest') WHERE _change = 'checkpoint';
    CREATE TABLE lake.files AS
        SELECT id, path, name, size, modified, modified_by FROM read_sharepoint_files('sharepoint://{folder}/**');
"""

MIRROR_RUN = """
    LOAD ducklake;
    ATTACH 'ducklake:{catalog}' AS lake (DATA_PATH '{data}');
    SET VARIABLE t = (SELECT token FROM lake.sync);
    CREATE TEMP TABLE c AS SELECT * FROM sharepoint_changes('sharepoint://{folder}/', token := getvariable('t'));
    DELETE FROM lake.files WHERE id IN (SELECT id FROM c WHERE _change <> 'checkpoint');
    INSERT INTO lake.files
        SELECT id, path, name, size, modified, modified_by FROM c WHERE _change = 'upsert' AND NOT is_folder;
    UPDATE lake.sync SET token = (SELECT token FROM c WHERE _change = 'checkpoint');
"""


def test_mirror_a_folder_into_ducklake(graph, scratch, sql, secret_sql, tmp_path):
    graph.upload(f"{scratch}/a.csv", b"n\n1\n")
    graph.upload(f"{scratch}/b.csv", b"n\n2\n")
    db = str(tmp_path / "mirror.db")
    fmt = {"catalog": tmp_path / "lake.ducklake", "data": tmp_path / "lake/", "folder": scratch}
    sql(secret_sql + MIRROR_ONCE.format(**fmt), db=db)
    listed = sql(f"{secret_sql} LOAD ducklake; ATTACH 'ducklake:{fmt['catalog']}' AS lake (DATA_PATH '{fmt['data']}');"
                 " SELECT string_agg(name, ',' ORDER BY name) FROM lake.files;", db=db).splitlines()
    assert listed[-1] == "a.csv,b.csv"

    graph.delete(f"{scratch}/a.csv", permanent=True)
    b_new, c_new = b"n\n2\n3\n4\n", b"n\n5\n"
    graph.upload(f"{scratch}/b.csv", b_new)
    graph.upload(f"{scratch}/c.csv", c_new)
    want = f"b.csv:{len(b_new)},c.csv:{len(c_new)}"
    # Delta trails writes by seconds: run the job until the mirror catches up
    # (each run is incremental, so running it again is harmless).
    deadline = time.time() + 120
    while True:
        out = sql(secret_sql + MIRROR_RUN.format(**fmt) + """
            SELECT string_agg(name || ':' || size, ',' ORDER BY name), count(*) FILTER (WHERE modified_by IS NULL)
            FROM lake.files;""", db=db).splitlines()
        if out[-1].startswith(want + "|") or time.time() > deadline:
            break
        time.sleep(10)
    assert out[-1] == want + "|0", out
