"""Delegated (user) auth, end to end against a REAL tenant.

* The device-code prompt test runs with the app-only credentials: the
  /devicecode endpoint answers for that app registration too, so everything up
  to the human signing in is exercised (and what reaches the terminal checked).
* The user round trip needs a delegated refresh token for a test user --
  SHAREPOINT_DELEGATED_CLIENT_ID and SHAREPOINT_USER_REFRESH_TOKEN, written by
  `scripts/delegated_login.py` into the git-ignored .env.sharepoint.delegated.
  Without them it skips.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.live

DELEGATED = ("SHAREPOINT_DELEGATED_CLIENT_ID", "SHAREPOINT_USER_REFRESH_TOKEN")


def _written_by_a_user(last_modified_by: dict) -> bool:
    """SharePoint attributes an app-only write to a pseudo-user named
    "SharePoint-App" (verified live 2026-10-14), so the presence of a `user`
    key proves nothing -- the name does."""
    user = last_modified_by.get("user") or {}
    return bool(user) and user.get("displayName") != "SharePoint-App"


def _require_delegated() -> None:
    missing = [n for n in DELEGATED if not os.environ.get(n)]
    if missing and os.environ.get("SHAREPOINT_REQUIRE_DELEGATED") == "1":
        pytest.fail(f"SHAREPOINT_REQUIRE_DELEGATED=1 but {', '.join(missing)} not set")
    if missing:
        pytest.skip(f"delegated credentials not configured -- run scripts/delegated_login.py ({', '.join(missing)})")


def _delegated_secret(name: str, persistent: bool = False) -> str:
    # getenv(): the SQL text never holds the refresh token.
    kind = "PERSISTENT SECRET" if persistent else "SECRET"
    return (f"CREATE OR REPLACE {kind} {name} (TYPE sharepoint, PROVIDER device_code, "
            "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_DELEGATED_CLIENT_ID'), "
            "REFRESH_TOKEN getenv('SHAREPOINT_USER_REFRESH_TOKEN'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")


def test_device_code_prompt_shows_only_url_and_code(graph, sql):
    """The real /devicecode round trip; nobody signs in, so the bounded wait
    gives up. The terminal gets one line: the verification URL and user code.
    Uses the public-client app when configured, else the app-only one (whose
    registration answers /devicecode too)."""
    client_env = ("SHAREPOINT_DELEGATED_CLIENT_ID" if os.environ.get("SHAREPOINT_DELEGATED_CLIENT_ID")
                  else "SHAREPOINT_CLIENT_ID")
    out = sql(
        "SET sharepoint_device_code_timeout_seconds = 12;"
        "CREATE SECRET sp_dc (TYPE sharepoint, PROVIDER device_code, TENANT_ID getenv('SHAREPOINT_TENANT_ID'), "
        f"CLIENT_ID getenv('{client_env}'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));"
        "SELECT file_size('sharepoint://anything.txt');",
        expect_error=True)
    prompt = [line for line in out.splitlines() if "to sign in, open" in line]
    assert len(prompt) == 1, out
    assert re.search(r"open https://\S+ and enter the code [A-Za-z0-9-]{1,16} \(valid for \d+ minutes\)",
                     prompt[0])
    assert "the device-code sign-in was not completed within 12 seconds" in out
    # No token- or device-code-shaped material is ever printed.
    assert "device_code=" not in out
    assert not re.search(r"eyJ[A-Za-z0-9_-]{10,}", out)


def test_user_writes_and_reads_through_a_persistent_delegated_secret(graph, scratch, sql):
    """A PERSISTENT device_code secret carrying the user's refresh token works
    across restarts with no prompt; files written through it are authored by
    the USER (not the app), and the token never reaches the output."""
    _require_delegated()
    refresh_token = os.environ["SHAREPOINT_USER_REFRESH_TOKEN"]

    out = sql(_delegated_secret("sp_user", persistent=True))
    assert refresh_token not in out

    # Fresh process: the stored secret applies, refreshed with no prompt.
    out = sql(f"SELECT write_blob('sharepoint://{scratch}/user.txt', 'written by a user'::BLOB);")
    assert "to sign in, open" not in out
    out = sql(f"SELECT content::VARCHAR FROM read_blob('sharepoint://{scratch}/user.txt');")
    assert out.splitlines()[-1] == "written by a user"

    # Graph (the app-only "other client") sees the file authored by a user.
    item = graph.stat(f"{scratch}/user.txt")
    assert item is not None
    assert _written_by_a_user(item.get("lastModifiedBy", {})), item.get("lastModifiedBy")

    # Parquet round trip under the user's identity.
    sql(f"COPY (SELECT range AS n FROM range(500)) TO 'sharepoint://{scratch}/u.parquet' (FORMAT parquet);")
    out = sql(f"SELECT sum(n) FROM read_parquet('sharepoint://{scratch}/u.parquet');")
    assert out.splitlines()[-1] == str(sum(range(500)))

    # Redacted in duckdb_secrets(), on every line of output.
    out = sql("SELECT secret_string FROM duckdb_secrets() WHERE name = 'sp_user';")
    assert "refresh_token=redacted" in out
    assert refresh_token not in out

    sql("DROP PERSISTENT SECRET sp_user;")


def test_user_identity_does_not_share_tokens_with_the_app(graph, scratch, sql):
    """Same secret name, app-only then delegated, in ONE process: the token
    cache is keyed by credential fingerprint, so the second secret must not be
    served the first one's token (the file's author proves whose token wrote)."""
    _require_delegated()
    app_secret = ("CREATE OR REPLACE SECRET sp_same (TYPE sharepoint, PROVIDER client_credentials, "
                  "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CLIENT_ID'), "
                  "CLIENT_SECRET getenv('SHAREPOINT_CLIENT_SECRET'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")
    sql(app_secret
        + f"SELECT write_blob('sharepoint://{scratch}/by_app.txt', 'a'::BLOB);"
        + _delegated_secret("sp_same")
        + f"SELECT write_blob('sharepoint://{scratch}/by_user.txt', 'u'::BLOB);")
    by_app = graph.stat(f"{scratch}/by_app.txt")["lastModifiedBy"]
    by_user = graph.stat(f"{scratch}/by_user.txt")["lastModifiedBy"]
    assert not _written_by_a_user(by_app), by_app
    assert _written_by_a_user(by_user), by_user


def test_authorization_code_fails_fast_without_a_browser(sql, isolated_home):
    """On a headless host PROVIDER authorization_code refuses at once -- before
    any port is bound or request sent -- and points at device_code, instead of
    waiting out the redirect timeout. Needs no credentials."""
    isolated_home.pop("DISPLAY", None)
    isolated_home.pop("WAYLAND_DISPLAY", None)
    out = sql("CREATE SECRET sp_ac (TYPE sharepoint, PROVIDER authorization_code, "
              "TENANT_ID 'contoso.onmicrosoft.com', CLIENT_ID '11111111-1111-1111-1111-111111111111', "
              "DRIVE_ID 'b!placeholder');"
              "SELECT file_size('sharepoint://anything.txt');", expect_error=True)
    assert "no browser is available on this machine" in out
    assert "PROVIDER device_code" in out


def test_rotated_refresh_token_is_persisted(graph, scratch, sql, isolated_home):
    """Entra rotates the refresh token on every refresh. Forced re-mints write
    the newest one back into the PERSISTENT secret (with its lineage), and a
    fresh process works from what was stored. Answers the open question of
    whether Entra rotates for this app: if it does not, the stored token is
    unchanged and the first assertion below says so."""
    _require_delegated()
    original = os.environ["SHAREPOINT_USER_REFRESH_TOKEN"]
    sql(_delegated_secret("sp_rot", persistent=True))
    sql(f"SELECT write_blob('sharepoint://{scratch}/rot.txt', 'x'::BLOB);")
    sql("SET sharepoint_debug_token_max_age_seconds = 60;"
        f"SELECT file_size('sharepoint://{scratch}/rot.txt');"
        f"SELECT file_size('sharepoint://{scratch}/rot.txt');")
    stored = (Path(isolated_home["HOME"]) / ".duckdb" / "stored_secrets" / "sp_rot.duckdb_secret").read_bytes()
    assert b"refresh_token_lineage" in stored, "no rotated refresh token was written back"
    assert original.encode() not in stored, "the stored refresh token was not replaced by the rotated one"
    out = sql(f"SELECT content::VARCHAR FROM read_blob('sharepoint://{scratch}/rot.txt');")
    assert out.splitlines()[-1] == "x"
    sql("DROP PERSISTENT SECRET sp_rot;")


@pytest.mark.skipif(os.environ.get("SHAREPOINT_INTERACTIVE") != "1",
                    reason="needs a human: set SHAREPOINT_INTERACTIVE=1 and run with `pytest -s`")
def test_interactive_device_code_round_trip(graph, scratch, duckdb_cli, isolated_home):
    """The headline path, with a human: a PERSISTENT device_code secret with NO
    refresh token prompts once (URL + code on the terminal), stores the refresh
    token (redacted), and a restarted process works without a prompt, writing
    as the user. Run:

        SHAREPOINT_INTERACTIVE=1 uv run --locked pytest -s -k interactive
    """
    if not os.environ.get("SHAREPOINT_DELEGATED_CLIENT_ID"):
        pytest.skip("SHAREPOINT_DELEGATED_CLIENT_ID not set")

    def run(statements: str, show_stderr: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run([str(duckdb_cli), "-noheader", "-list"], input=statements, text=True,
                              stdout=subprocess.PIPE, stderr=None if show_stderr else subprocess.PIPE,
                              timeout=1000, env=isolated_home)

    first = run("CREATE PERSISTENT SECRET sp_human (TYPE sharepoint, PROVIDER device_code, "
                "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_DELEGATED_CLIENT_ID'), "
                "DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));"
                f"SELECT write_blob('sharepoint://{scratch}/human.txt', 'signed in'::BLOB);",
                show_stderr=True)  # the prompt goes straight to the terminal
    assert first.returncode == 0, first.stdout

    stored = (Path(isolated_home["HOME"]) / ".duckdb" / "stored_secrets" / "sp_human.duckdb_secret").read_bytes()
    assert b"refresh_token" in stored and b"refresh_token_lineage" in stored

    second = run("SELECT secret_string FROM duckdb_secrets() WHERE name = 'sp_human';"
                 f"SELECT content::VARCHAR FROM read_blob('sharepoint://{scratch}/human.txt');")
    assert second.returncode == 0, second.stderr
    assert "to sign in, open" not in second.stderr, "a restart prompted again"
    assert "refresh_token=redacted" in second.stdout
    assert second.stdout.strip().splitlines()[-1] == "signed in"
    assert _written_by_a_user(graph.stat(f"{scratch}/human.txt")["lastModifiedBy"])
    run("DROP PERSISTENT SECRET sp_human;")
