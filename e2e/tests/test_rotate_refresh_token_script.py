"""Offline tests for scripts/rotate_refresh_token.py -- the scheduled job that
redeems the CI user's refresh token and writes the rotated one back to the
repo secret. post() and the `gh` runner are stubbed; nothing touches the
network or GitHub."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "rotate_refresh_token.py"
CLIENT = "11111111-2222-3333-4444-555555555555"
# Built by concatenation so check_no_credentials.sh's token-shape grep never
# sees a literal refresh token in this file.
OLD = "0.AXoA" + "o" * 300 + ".old"
NEW = "0.AXoA" + "n" * 300 + ".new"
PAT = "github_pat_" + "p" * 40


@pytest.fixture(scope="module")
def rt():
    spec = importlib.util.spec_from_file_location("rotate_refresh_token", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def env(**over):
    e = {"SHAREPOINT_TENANT_ID": "contoso.onmicrosoft.com", "SHAREPOINT_DELEGATED_CLIENT_ID": CLIENT,
         "SHAREPOINT_USER_REFRESH_TOKEN": OLD, "SECRETS_ROTATION_TOKEN": PAT, "GITHUB_ACTIONS": "true"}
    e.update(over)
    return {k: v for k, v in e.items() if v is not None}


class Post:
    def __init__(self, *replies):
        self.replies = list(replies)
        self.calls = []

    def __call__(self, url, form):
        self.calls.append((url, form))
        return self.replies.pop(0)


class Gh:
    """Records each `gh` invocation: argv, stdin and the environment."""

    def __init__(self, returncode=0, stderr=""):
        self.returncode, self.stderr = returncode, stderr
        self.calls = []

    def __call__(self, argv, stdin, env):
        self.calls.append({"argv": list(argv), "stdin": stdin, "env": dict(env)})
        return self.returncode, self.stderr


def ok(token=NEW):
    return 200, {"access_token": "AT", "token_type": "Bearer", "refresh_token": token}


def run(rt, post, gh, environment=None, dry_run=False):
    out = []
    rt.rotate(environment or env(), dry_run=dry_run, repo="DataZooDE/duckdb-sharepoint",
              post_fn=post, gh_fn=gh, say=out.append)
    return out


def test_redeems_as_public_client_and_writes_via_stdin(rt):
    post, gh = Post(ok()), Gh()
    out = run(rt, post, gh)
    url, form = post.calls[0]
    assert url == "https://login.microsoftonline.com/contoso.onmicrosoft.com/oauth2/v2.0/token"
    assert form["grant_type"] == "refresh_token"
    assert form["refresh_token"] == OLD
    assert form["client_id"] == CLIENT
    assert "client_secret" not in form
    assert form["scope"] == ("https://graph.microsoft.com/Files.ReadWrite.All "
                             "https://graph.microsoft.com/Sites.ReadWrite.All offline_access")
    assert len(gh.calls) == 1
    call = gh.calls[0]
    assert call["argv"] == ["gh", "secret", "set", "SHAREPOINT_USER_REFRESH_TOKEN",
                            "--repo", "DataZooDE/duckdb-sharepoint"]
    assert call["stdin"] == NEW
    assert all(NEW not in a and PAT not in a for a in call["argv"])
    assert call["env"]["GH_TOKEN"] == PAT
    # The child gets the PAT and nothing else secret.
    assert "SHAREPOINT_USER_REFRESH_TOKEN" not in call["env"]
    assert "SECRETS_ROTATION_TOKEN" not in call["env"]
    assert any("updated" in line for line in out)


def test_add_mask_is_the_first_output_and_no_token_is_printed_otherwise(rt):
    out = run(rt, Post(ok()), Gh())
    assert out[0] == f"::add-mask::{NEW}"
    for line in out[1:]:
        assert NEW not in line and OLD not in line and PAT not in line


def test_outside_actions_no_mask_line_and_no_token_printed(rt):
    out = run(rt, Post(ok()), Gh(), environment=env(GITHUB_ACTIONS=None), dry_run=True)
    assert out
    for line in out:
        assert NEW not in line and OLD not in line and "::add-mask::" not in line


def test_dry_run_redeems_but_writes_nothing(rt):
    post, gh = Post(ok()), Gh()
    out = run(rt, post, gh, dry_run=True)
    assert len(post.calls) == 1
    assert gh.calls == []
    assert any("dry run" in line and "rotated" in line for line in out)


def test_dry_run_needs_no_pat(rt):
    gh = Gh()
    run(rt, Post(ok()), gh, environment=env(SECRETS_ROTATION_TOKEN=None), dry_run=True)
    assert gh.calls == []


def test_missing_pat_fails_loudly_before_redeeming(rt):
    post, gh = Post(), Gh()
    with pytest.raises(rt.RotateError) as e:
        run(rt, post, gh, environment=env(SECRETS_ROTATION_TOKEN=None))
    assert "SECRETS_ROTATION_TOKEN" in str(e.value)
    assert "Secrets" in str(e.value)  # names the permission to grant
    assert post.calls == [] and gh.calls == []


def test_invalid_grant_names_the_relogin_fix(rt):
    post, gh = Post((400, {"error": "invalid_grant", "error_codes": [700082],
                           "error_description": "SECRET-DETAIL"})), Gh()
    with pytest.raises(rt.RotateError) as e:
        run(rt, post, gh)
    msg = str(e.value)
    assert "make delegated_login" in msg
    assert "SHAREPOINT_USER_REFRESH_TOKEN" in msg
    assert "AADSTS700082" in msg
    assert "SECRET-DETAIL" not in msg and OLD not in msg
    assert gh.calls == []


def test_other_token_endpoint_errors_fail_without_writing(rt):
    gh = Gh()
    with pytest.raises(rt.RotateError):
        run(rt, Post((503, {})), gh)
    assert gh.calls == []


def test_missing_refresh_token_in_response_fails(rt):
    gh = Gh()
    with pytest.raises(rt.RotateError) as e:
        run(rt, Post((200, {"access_token": "AT"})), gh)
    assert "offline_access" in str(e.value)
    assert gh.calls == []


@pytest.mark.parametrize("bad", ["has'quote" + "a" * 30, "has space" + "a" * 30, "line\nbreak" + "a" * 30,
                                 "$(id)" + "a" * 30, "short"])
def test_a_badly_shaped_rotated_token_is_not_written(rt, bad):
    gh = Gh()
    with pytest.raises(rt.RotateError) as e:
        run(rt, Post(ok(bad)), gh)
    assert bad not in str(e.value)
    assert gh.calls == []


def test_an_unrotated_token_is_not_rewritten(rt):
    gh = Gh()
    out = run(rt, Post(ok(OLD)), gh)
    assert gh.calls == []
    assert any("not rotate" in line for line in out)


@pytest.mark.parametrize("over", [
    {"SHAREPOINT_USER_REFRESH_TOKEN": None},
    {"SHAREPOINT_USER_REFRESH_TOKEN": "bad token with spaces and more"},
    {"SHAREPOINT_DELEGATED_CLIENT_ID": "not-a-guid"},
    {"SHAREPOINT_TENANT_ID": "../evil"},
    {"SHAREPOINT_LOGIN_ENDPOINT": "https://login.microsoftonline.com.evil.example"},
])
def test_inputs_are_validated_before_any_request(rt, over):
    post, gh = Post(), Gh()
    with pytest.raises(Exception):
        run(rt, post, gh, environment=env(**over))
    assert post.calls == [] and gh.calls == []


def test_gh_failure_is_loud_and_scrubbed(rt):
    gh = Gh(returncode=1, stderr=f"HTTP 403: Resource not accessible by personal access token {NEW}")
    with pytest.raises(rt.RotateError) as e:
        run(rt, Post(ok()), gh)
    msg = str(e.value)
    assert "403" in msg and NEW not in msg
    assert "Secrets" in msg


def test_bad_repo_is_refused(rt):
    post = Post()
    with pytest.raises(rt.RotateError):
        rt.rotate(env(), dry_run=False, repo="evil; rm -rf /", post_fn=post, gh_fn=Gh(), say=lambda s: None)
    assert post.calls == []
