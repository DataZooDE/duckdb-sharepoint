"""read_onenote_pages() (#102) and COPY ... (FORMAT onenote) (#105).
Microsoft restricts app-only access to the OneNote API; this test records
what the tenant does: app-only either reads or is refused with the
extension's explanation, and a signed-in user with Notes.Read reads the
site's pages (skipped when the delegated app may not ask for Notes.Read).
With Notes.ReadWrite and SHAREPOINT_ONENOTE_NOTEBOOK (an existing notebook on
the test site), the user creates a page, appends to it and reads it back."""

from __future__ import annotations

import os
import time
import uuid

import pytest

pytestmark = pytest.mark.live


def _run(sql, statements):
    try:
        return True, sql(statements)
    except AssertionError as e:
        return False, str(e)


def test_app_only(sql, secret_sql):
    ok, out = _run(sql, f"{secret_sql} SELECT 'pages', count(*) >= 0 FROM read_onenote_pages();")
    if ok:
        assert out.splitlines()[-1] == "pages|true", out
        print("app-only OneNote access is allowed on this tenant")
    else:
        assert "restricts app-only access to OneNote" in out, out


def _user_secret(notes_scope: str) -> str:
    for n in ("SHAREPOINT_DELEGATED_CLIENT_ID", "SHAREPOINT_USER_REFRESH_TOKEN"):
        if not os.environ.get(n):
            pytest.skip(f"{n} not set")
    return ("CREATE SECRET me (TYPE sharepoint, PROVIDER device_code, TENANT_ID getenv('SHAREPOINT_TENANT_ID'), "
            "CLIENT_ID getenv('SHAREPOINT_DELEGATED_CLIENT_ID'), REFRESH_TOKEN getenv('SHAREPOINT_USER_REFRESH_TOKEN'), "
            f"DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'), GRAPH_SCOPE 'https://graph.microsoft.com/{notes_scope} "
            "https://graph.microsoft.com/Files.ReadWrite.All https://graph.microsoft.com/Sites.ReadWrite.All "
            "offline_access');")


def _skip_without_consent(ok: bool, out: str, scope: str) -> None:
    # Only Entra's own "not consented" replies: "consent" alone also matches
    # other hints (a Graph 403's "with admin consent"), which then hid the real
    # error behind this skip (2026-11-09).
    if not ok and ("AADSTS65001" in out or "have not been consented" in out or "invalid_scope" in out):
        pytest.skip(f"the delegated app may not ask for {scope} (no consent)")


def test_signed_in_user(sql):
    secret = _user_secret("Notes.Read")
    ok, out = _run(sql, f"{secret} SELECT 'pages', count(*) >= 0, count(*) FILTER (WHERE modified < created) "
                        "FROM read_onenote_pages();")
    _skip_without_consent(ok, out, "Notes.Read")
    assert ok, out
    assert out.splitlines()[-1] == "pages|true|0", out


def test_signed_in_user_creates_and_appends_to_a_page(sql):
    notebook = os.environ.get("SHAREPOINT_ONENOTE_NOTEBOOK")
    if not notebook:
        pytest.skip("SHAREPOINT_ONENOTE_NOTEBOOK not set (an existing notebook on the test site)")
    assert "'" not in notebook
    secret = _user_secret("Notes.ReadWrite")
    # One fixed section (created by the first run's COPY); a page per run.
    # OneNote has no API to delete a section, and the pages are small.
    target = f"{notebook}/duckdb-e2e"
    title = f"e2e {uuid.uuid4().hex[:12]}"
    ok, out = _run(sql, f"{secret} COPY (SELECT '{title}' AS title, '<p>first &amp; <b>bold</b></p>' AS content) "
                        f"TO '{target}' (FORMAT onenote);")
    _skip_without_consent(ok, out, "Notes.ReadWrite")
    assert ok, out

    # A new page can take a moment to be listed.
    pages = f"read_onenote_pages(notebook := '{notebook}', section := 'duckdb-e2e', content := true)"
    for attempt in range(8):
        out = sql(f"{secret} SELECT 'page', count(*) FROM {pages} WHERE title = '{title}';")
        if out.splitlines()[-1] == "page|1":
            break
        time.sleep(5 * (attempt + 1))
    assert out.splitlines()[-1] == "page|1", out

    # Append to it by its id, then read the content back.
    sql(f"{secret} COPY (SELECT id, '<p>second line</p>' AS content FROM {pages} WHERE title = '{title}') "
        f"TO '{target}' (FORMAT onenote);")
    for attempt in range(8):
        out = sql(f"{secret} SELECT 'content', content LIKE '%first &amp; %bold%', content LIKE '%second line%' "
                  f"FROM {pages} WHERE title = '{title}';")
        if out.splitlines()[-1] == "content|true|true":
            break
        time.sleep(5 * (attempt + 1))
    assert out.splitlines()[-1] == "content|true|true", out
