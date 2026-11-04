"""sharepoint_permissions() (#98) against the REAL drive: a file's sharing
links with their scope, what it inherits from its site, and a glob over
several files. An 'anyone' link only where the tenant allows them (a tenant
setting); a direct grant to a person needs a user to invite, which CI has not."""

from __future__ import annotations

import pytest

from helpers.graph import GRAPH

pytestmark = pytest.mark.live


def _link(graph, path: str, scope: str):
    item = graph.stat(path)
    return graph._req("POST", f"{GRAPH}/drives/{graph.drive_id}/items/{item['id']}/createLink",
                      json={"type": "view", "scope": scope})


def test_links_and_inherited_grants(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/a.csv", b"n\n1\n")
    graph.upload(f"{scratch}/b.csv", b"n\n2\n")
    r = _link(graph, f"{scratch}/a.csv", "organization")
    r.raise_for_status()
    link = r.json()["link"]["webUrl"]

    out = sql(f"""{secret_sql}
        SELECT count(*) FROM sharepoint_permissions('sharepoint://{scratch}/a.csv')
        WHERE granted_by = 'link' AND link_scope = 'organization' AND link_url = '{link}' AND list_contains(roles, 'read');
        SELECT count(*) > 0, count(*) FILTER (WHERE grantee IS NULL) FROM sharepoint_permissions('sharepoint://{scratch}/a.csv')
        WHERE granted_by = 'inherited';
        SELECT string_agg(DISTINCT parse_filename(path), ',' ORDER BY parse_filename(path))
        FROM sharepoint_permissions('sharepoint://{scratch}/*.csv');
        SELECT count(*) > 0 FROM sharepoint_permissions('sharepoint://{scratch}') WHERE granted_by = 'inherited';
    """).splitlines()
    assert out[-4] == "1", out
    assert out[-3] == "true|0", out  # inherited grants, each with its grantee
    assert out[-2] == "a.csv,b.csv", out
    assert out[-1] == "true", out  # a folder by its path

    out = sql(f"{secret_sql} SELECT * FROM sharepoint_permissions('sharepoint://{scratch}/missing.csv');",
              expect_error=True)
    assert "no such file or folder" in out, out


def test_anyone_link(graph, scratch, sql, secret_sql):
    graph.upload(f"{scratch}/open.csv", b"n\n1\n")
    r = _link(graph, f"{scratch}/open.csv", "anonymous")
    if r.status_code in (400, 403):
        pytest.skip(f"the tenant does not allow 'anyone' links (HTTP {r.status_code})")
    r.raise_for_status()
    out = sql(f"""{secret_sql}
        SELECT count(*) FROM sharepoint_permissions('sharepoint://{scratch}/open.csv')
        WHERE granted_by = 'link' AND link_scope = 'anonymous';
    """).splitlines()
    assert out[-1] == "1", out
