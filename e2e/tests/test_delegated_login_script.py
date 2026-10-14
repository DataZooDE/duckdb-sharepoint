"""Offline tests for scripts/delegated_login.py -- the script that handles a
user's refresh token. post() is stubbed; nothing touches the network."""

from __future__ import annotations

import importlib.util
import os
import stat
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "delegated_login.py"
CLIENT = "11111111-2222-3333-4444-555555555555"
TOKEN = "0.AXoA" + "a" * 300 + ".b-c_d"


@pytest.fixture(scope="module")
def dl():
    spec = importlib.util.spec_from_file_location("delegated_login", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def env(**over):
    e = {"SHAREPOINT_TENANT_ID": "contoso.onmicrosoft.com", "SHAREPOINT_DELEGATED_CLIENT_ID": CLIENT}
    e.update(over)
    return e


def grant(**over):
    g = {"device_code": "DEVICE-SECRET", "user_code": "ABCD1234",
         "verification_uri": "https://login.microsoft.com/device", "expires_in": 900, "interval": 5}
    g.update(over)
    return g


class Stub:
    """Replies in order; records requests."""

    def __init__(self, *replies):
        self.replies = list(replies)
        self.calls = []

    def __call__(self, url, form):
        self.calls.append((url, form))
        return self.replies.pop(0)


def run(dl, stub, environment=None):
    said = []
    t = [0.0]
    sleeps = []

    def sleep(s):
        sleeps.append(s)
        t[0] += s

    values = dl.login(environment or env(), post_fn=stub, sleep=sleep, clock=lambda: t[0], say=said.append)
    return values, said, sleeps


def test_happy_path_prints_only_url_and_code(dl):
    stub = Stub((200, grant()),
                (400, {"error": "authorization_pending", "error_codes": [70016]}),
                (400, {"error": "slow_down"}),
                (200, {"access_token": "AT", "refresh_token": TOKEN}))
    values, said, sleeps = run(dl, stub)
    assert said == ["Open https://login.microsoft.com/device and enter the code ABCD1234"]
    assert values["SHAREPOINT_USER_REFRESH_TOKEN"] == TOKEN
    assert sleeps == [5, 5, 10]  # slow_down adds 5 s
    assert "client_secret" not in stub.calls[0][1]
    assert stub.calls[1][1]["grant_type"] == "urn:ietf:params:oauth:grant-type:device_code"


@pytest.mark.parametrize("bad", [
    {"verification_uri": "https://phish.example/device"},
    {"verification_uri": "http://login.microsoft.com/device"},
    {"verification_uri": "https://login.microsoft.com/de\nvice"},
    {"user_code": "AB\nCD"},
    {"user_code": "ABCD\x1b[2J"},
    {"user_code": "A" * 17},
])
def test_hostile_prompt_fields_are_refused(dl, bad):
    with pytest.raises(dl.LoginError):
        run(dl, Stub((200, grant(**bad))))


@pytest.mark.parametrize("over", [
    {"SHAREPOINT_DELEGATED_CLIENT_ID": "not-a-guid"},
    {"SHAREPOINT_DELEGATED_CLIENT_ID": CLIENT + "'; rm -rf ~"},
    {"SHAREPOINT_TENANT_ID": "../evil"},
    {"SHAREPOINT_LOGIN_ENDPOINT": "https://login.microsoftonline.com.evil.example"},
])
def test_inputs_are_validated_before_any_request(dl, over):
    stub = Stub()
    with pytest.raises(dl.LoginError):
        run(dl, stub, env(**over))
    assert stub.calls == []


@pytest.mark.parametrize("token", ["has'quote" + "a" * 30, "has space" + "a" * 30, "$(id)" + "a" * 30])
def test_a_token_unsafe_for_a_sourced_file_is_not_written(dl, token):
    stub = Stub((200, grant()), (200, {"access_token": "AT", "refresh_token": token}))
    with pytest.raises(dl.LoginError):
        run(dl, stub)


def test_errors_name_the_fix_and_never_echo_the_description(dl):
    stub = Stub((200, grant()), (401, {"error": "invalid_client", "error_codes": [7000218],
                                       "error_description": "SECRET-DETAIL"}))
    with pytest.raises(dl.LoginError) as e:
        run(dl, stub)
    assert "public client flows" in str(e.value)
    assert "SECRET-DETAIL" not in str(e.value)


def test_redirects_are_not_followed(dl):
    handler = dl._NoRedirect()
    assert handler.redirect_request(None, None, 307, "", {}, "https://evil.example/") is None


def test_env_file_is_0600_quoted_and_replaces_a_wider_tmp(dl, tmp_path):
    out = tmp_path / ".env.sharepoint.delegated"
    stale = tmp_path / ".env.sharepoint.delegated.tmp"
    stale.write_text("old")
    os.chmod(stale, 0o644)
    dl.write_env(out, {"SHAREPOINT_USER_REFRESH_TOKEN": TOKEN})
    assert stat.S_IMODE(out.stat().st_mode) == 0o600
    assert f"SHAREPOINT_USER_REFRESH_TOKEN='{TOKEN}'" in out.read_text()
    assert not stale.exists()
