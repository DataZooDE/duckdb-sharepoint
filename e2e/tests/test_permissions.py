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

    # Every row, to see Graph's answer when an assertion fails.
    rows = sql(f"""{secret_sql}
        SELECT granted_by || '|' || coalesce(grantee_type, '-') || '|' || coalesce(link_scope, '-') || '|' ||
               list_sort(roles)::VARCHAR || '|' || coalesce(link_url, '-') || '|' || coalesce(inherited_from, '-')
        FROM sharepoint_permissions('sharepoint://{scratch}/a.csv') ORDER BY 1;
    """).splitlines()[1:]
    # Seen live (hld §7a): the listing gives the link without its webUrl, and
    # the site's groups without inheritedFrom -- so they read as 'direct'.
    links = [r for r in rows if r.startswith("link|") and "|organization|" in r and "[read]" in r]
    assert len(links) == 1, (link, rows)
    assert any(r.split("|")[1] in ("group", "site_group") for r in rows), rows  # the site's groups
    out = sql(f"""{secret_sql}
        SELECT string_agg(DISTINCT parse_filename(path), ',' ORDER BY parse_filename(path))
        FROM sharepoint_permissions('sharepoint://{scratch}/*.csv');
        SELECT count(*) > 0 FROM sharepoint_permissions('sharepoint://{scratch}');
    """).splitlines()
    assert out[-2:] == ["a.csv,b.csv", "true"], out

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


def test_a_glob_audits_folders_too(graph, scratch, sql, secret_sql):
    """A folder shared with everyone exposes its whole tree; a glob that saw
    only files let an audit come back clean (exploratory testing of
    duckdb-gdrive, 2026-11-07, checked here)."""
    graph.mkdir(f"{scratch}/shared")
    graph.upload(f"{scratch}/shared/a.csv", b"n\n1\n")
    graph.upload(f"{scratch}/top.csv", b"n\n1\n")
    out = sql(f"""{secret_sql}
        SELECT string_agg(DISTINCT parse_filename(path), ',' ORDER BY parse_filename(path))
        FROM sharepoint_permissions('sharepoint://{scratch}/*');""").splitlines()
    assert out[-1] == "shared,top.csv", out
