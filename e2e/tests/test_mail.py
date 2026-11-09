"""read_outlook_messages() (#126) against the REAL tenant. Needs Mail.Read,
which the CI apps do not have by default, so the reading tests skip unless set
up:
  SHAREPOINT_MAIL_USER  a mailbox the CI app may read (Mail.Read, application,
                        admin consent; an application access policy may limit
                        the app to this mailbox)
A signed-in user reads their own inbox when the delegated app may ask for
Mail.Read (skipped otherwise)."""

from __future__ import annotations

import os

import pytest

pytestmark = pytest.mark.live


def _run(sql, statements):
    try:
        return True, sql(statements)
    except AssertionError as e:
        return False, str(e)


def test_app_only_without_a_user_is_explained(sql, secret_sql):
    # An app has no mailbox of its own: /me is refused, with what to do.
    ok, out = _run(sql, f"{secret_sql} SELECT count(*) FROM read_outlook_messages(folder := 'inbox');")
    assert not ok, out
    assert "user :=" in out or "needs Mail.Read" in out, out


def test_app_only_reads_a_mailbox(sql, secret_sql):
    user = os.environ.get("SHAREPOINT_MAIL_USER")
    if not user:
        pytest.skip("SHAREPOINT_MAIL_USER not set")
    assert "'" not in user
    mail = f"read_outlook_messages(user := '{user}', folder := 'inbox')"
    out = sql(f"""{secret_sql}
        SELECT 'all', count(*) >= 0, count(*) FILTER (WHERE id IS NULL OR received IS NULL) FROM {mail};
        SELECT 'newest first', bool_and(received >= next) FROM (
            SELECT received, lead(received) OVER () AS next FROM (SELECT received FROM {mail} LIMIT 30)) WHERE next IS NOT NULL;
        SELECT 'limit', count(*) <= 3 FROM (SELECT * FROM {mail} LIMIT 3);
        SELECT 'future', count(*) FROM read_outlook_messages(user := '{user}', since := now()::TIMESTAMP + INTERVAL 1 DAY);
        SELECT 'body', count(*) FILTER (WHERE body LIKE '%<html%') FROM (
            SELECT body FROM read_outlook_messages(user := '{user}', folder := 'inbox', body := true) LIMIT 5);
    """).splitlines()
    assert "all|true|0" in out, out
    assert "newest first|false" not in out, out  # NULL when the inbox has < 2 messages
    assert "limit|true" in out, out
    assert "future|0" in out, out
    assert "body|0" in out, out  # text, not HTML


def test_signed_in_user_reads_their_inbox(sql):
    for n in ("SHAREPOINT_DELEGATED_CLIENT_ID", "SHAREPOINT_USER_REFRESH_TOKEN"):
        if not os.environ.get(n):
            pytest.skip(f"{n} not set")
    secret = ("CREATE SECRET me (TYPE sharepoint, PROVIDER device_code, TENANT_ID getenv('SHAREPOINT_TENANT_ID'), "
              "CLIENT_ID getenv('SHAREPOINT_DELEGATED_CLIENT_ID'), REFRESH_TOKEN getenv('SHAREPOINT_USER_REFRESH_TOKEN'), "
              "DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'), GRAPH_SCOPE 'https://graph.microsoft.com/Mail.Read "
              "https://graph.microsoft.com/Files.ReadWrite.All https://graph.microsoft.com/Sites.ReadWrite.All "
              "offline_access');")
    ok, out = _run(sql, f"{secret} SELECT 'inbox', count(*) >= 0, count(*) FILTER (WHERE id IS NULL) "
                        "FROM (SELECT * FROM read_outlook_messages(folder := 'inbox') LIMIT 20);")
    if not ok and ("AADSTS65001" in out or "have not been consented" in out or "invalid_scope" in out):
        pytest.skip("the delegated app may not ask for Mail.Read (no consent)")
    if not ok and "no Exchange Online mailbox" in out:
        pytest.skip("the test user has no mailbox")
    assert ok, out
    assert out.splitlines()[-1] == "inbox|true|0", out
