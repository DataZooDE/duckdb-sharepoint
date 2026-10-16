"""The harness's own Graph calls survive transient failures.

CI run 38550327207 failed a test that had nothing wrong with it: the
harness's upload PUT got "504 Gateway Timeout" from graph.microsoft.com and
raised. The helper retried only 429/503 responses and never a transport
error -- a read timeout or a reset failed even a GET.

Every helper operation is safe to repeat: upload is a PUT by path (a replay
rewrites the same bytes), mkdir uses conflictBehavior "replace", stat and
children are GETs, and a replayed permanentDelete answers 404, which delete()
already treats as done. So unlike duckdb-gdrive's harness (which needs
reserved ids, because Drive lets two files share a name) a plain retry is
correct here.

The fault is injected around the REAL request, against the real tenant: the
wrapper either loses the response after Graph applied the request, or
returns Graph's 504 without sending. Nothing about Graph is simulated beyond
that one status line.
"""

from __future__ import annotations

import pytest
import requests

import helpers.graph as graph_mod

pytestmark = pytest.mark.live


def _fault_first(monkeypatch, method: str, fault: str) -> list[str]:
    """Make the first `method` request fail with `fault`."""
    real = requests.request
    seen: list[str] = []

    def flaky(m, url, **kw):
        if m.upper() == method and not seen:
            seen.append(url)
            if fault == "lost-after-applied":
                real(m, url, **kw)  # Graph applies it; the answer is dropped
                raise requests.exceptions.ReadTimeout("injected: response lost")
            if fault == "reset":
                raise requests.exceptions.ConnectionError("injected: connection reset")
            resp = requests.models.Response()
            resp.status_code = int(fault)
            resp._content = b'{"error":{"code":"UnknownError","message":"injected"}}'
            resp.url = url
            return resp
        return real(m, url, **kw)

    monkeypatch.setattr(graph_mod.requests, "request", flaky)
    return seen


@pytest.mark.parametrize("fault", ["lost-after-applied", "reset", "504", "502", "500"])
def test_upload_survives_a_transient_failure(graph, scratch, monkeypatch, fault):
    payload = b"a\n1\n"
    seen = _fault_first(monkeypatch, "PUT", fault)

    item = graph.upload(f"{scratch}/flaky.csv", payload)

    assert seen, "the fault was never injected -- the test proves nothing"
    monkeypatch.undo()
    named = [c for c in graph.children(scratch) if c["name"] == "flaky.csv"]
    assert [c["id"] for c in named] == [item["id"]]
    assert int(named[0]["size"]) == len(payload)


@pytest.mark.parametrize("fault", ["lost-after-applied", "504"])
def test_mkdir_survives_a_transient_failure(graph, scratch, monkeypatch, fault):
    seen = _fault_first(monkeypatch, "POST", fault)

    graph.mkdir(f"{scratch}/flaky-dir")

    assert seen, "the fault was never injected -- the test proves nothing"
    monkeypatch.undo()
    named = [c for c in graph.children(scratch) if c["name"] == "flaky-dir"]
    assert len(named) == 1 and "folder" in named[0]


def test_stat_survives_a_reset(graph, scratch, monkeypatch):
    graph.upload(f"{scratch}/here.csv", b"x\n")
    seen = _fault_first(monkeypatch, "GET", "reset")

    assert graph.stat(f"{scratch}/here.csv") is not None
    assert seen


def test_a_client_error_is_not_retried(graph, scratch, monkeypatch):
    """4xx means the request is wrong; repeating it only hides that."""
    real = requests.request
    sent: list[str] = []

    def count(m, url, **kw):
        if m.upper() == "PUT":
            sent.append(url)
        return real(m, url, **kw)

    monkeypatch.setattr(graph_mod.requests, "request", count)
    with pytest.raises(requests.HTTPError):
        graph.upload(f"{scratch}/bad:name?.csv", b"x\n")  # invalid name -> 400
    assert len(sent) == 1, f"a 4xx was retried {len(sent) - 1} time(s)"
