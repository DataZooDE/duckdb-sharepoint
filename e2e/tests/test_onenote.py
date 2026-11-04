"""read_onenote_pages() (#102). Microsoft restricts app-only access to the
OneNote API; this test records what the tenant does: app-only either reads
or is refused with the extension's explanation, and a signed-in user with
Notes.Read reads the site's pages (skipped when the delegated app may not
ask for Notes.Read)."""

from __future__ import annotations

import os

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


def test_signed_in_user(sql):
    for n in ("SHAREPOINT_DELEGATED_CLIENT_ID", "SHAREPOINT_USER_REFRESH_TOKEN"):
        if not os.environ.get(n):
            pytest.skip(f"{n} not set")
    secret = ("CREATE SECRET me (TYPE sharepoint, PROVIDER device_code, TENANT_ID getenv('SHAREPOINT_TENANT_ID'), "
              "CLIENT_ID getenv('SHAREPOINT_DELEGATED_CLIENT_ID'), REFRESH_TOKEN getenv('SHAREPOINT_USER_REFRESH_TOKEN'), "
              "DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'), GRAPH_SCOPE 'https://graph.microsoft.com/Notes.Read "
              "https://graph.microsoft.com/Files.ReadWrite.All https://graph.microsoft.com/Sites.ReadWrite.All "
              "offline_access');")
    ok, out = _run(sql, f"{secret} SELECT 'pages', count(*) >= 0, count(*) FILTER (WHERE modified < created) "
                        "FROM read_onenote_pages();")
    if not ok and ("AADSTS65001" in out or "consent" in out.lower() or "invalid_scope" in out):
        pytest.skip("the delegated app may not ask for Notes.Read (no consent)")
    assert ok, out
    assert out.splitlines()[-1] == "pages|true|0", out
