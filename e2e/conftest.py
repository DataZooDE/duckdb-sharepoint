"""Shared fixtures for the live harness (ported from duckdb-gdrive's e2e/).

Everything provisions against a REAL Microsoft Graph tenant. Without
credentials the suite skips with a message saying what to set, so a developer
without a tenant still gets a clean run.

Credentials: SHAREPOINT_TENANT_ID / _CLIENT_ID / _CLIENT_SECRET / _DRIVE_ID in
the environment, or in a git-ignored `.env.sharepoint` at the repo root.
"""

from __future__ import annotations

import os
import subprocess
import uuid
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_dotenv() -> None:
    """Load .env.sharepoint without a dependency. The environment wins."""
    env_file = REPO_ROOT / ".env.sharepoint"
    if not env_file.exists():
        return
    for line in env_file.read_text().splitlines():
        line = line.strip().removeprefix("export ").strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def scrub(text: str) -> str:
    """Remove credential values from text that may end up in a failure report
    (and so in CI logs)."""
    for name in ("SHAREPOINT_CLIENT_SECRET", "SHAREPOINT_CLIENT_ID",):
        value = os.environ.get(name)
        if value:
            text = text.replace(value, f"<{name}>")
    return text


_load_dotenv()

from helpers.graph import Graph, credentials_available  # noqa: E402
from helpers.sweep import SCRATCH_ROOT  # noqa: E402


@pytest.fixture(scope="session")
def graph() -> Graph:
    if not credentials_available():
        pytest.skip("live Graph credentials not configured -- set SHAREPOINT_TENANT_ID, "
                    "SHAREPOINT_CLIENT_ID, SHAREPOINT_CLIENT_SECRET, SHAREPOINT_DRIVE_ID")
    return Graph.from_env()


@pytest.fixture
def scratch(graph: Graph):
    """A per-test scratch folder path, permanently deleted on teardown.

    Per test so a failure cannot leak state into the next one and concurrent
    runs never collide; under one root so `make sweep_orphans` can find leaks.
    """
    path = f"{SCRATCH_ROOT}/run-{uuid.uuid4().hex[:12]}"
    if graph.stat(SCRATCH_ROOT) is None:
        graph.mkdir(SCRATCH_ROOT)
    graph.mkdir(path)
    try:
        yield path
    finally:
        try:
            graph.delete(path, permanent=True)
        except Exception as e:  # teardown must not mask a test failure
            print(f"WARNING: could not clean up scratch {path}: {e}")


class RedactedSQL(str):
    """A str whose repr hides it. pytest prints a failing test's fixture
    arguments in the traceback -- for the secret fixture that is the client
    secret, straight into CI logs. Found when a failing run printed it."""

    def __repr__(self) -> str:
        return "<RedactedSQL: CREATE SECRET ... (credentials hidden)>"


@pytest.fixture(scope="session")
def secret_sql(graph: Graph) -> str:
    """CREATE SECRET for the test principal. Never written to a tracked file,
    and never shown in a traceback (RedactedSQL)."""
    return RedactedSQL(
        "CREATE OR REPLACE SECRET sp_e2e (TYPE sharepoint, PROVIDER client_credentials, "
        f"TENANT_ID '{graph.tenant}', CLIENT_ID '{graph.client_id}', "
        f"CLIENT_SECRET '{graph.client_secret}', DRIVE_ID '{graph.drive_id}');"
    )


@pytest.fixture(scope="session")
def duckdb_cli() -> Path:
    """The built duckdb shell with the extension statically linked."""
    exe = REPO_ROOT / "build" / "release" / "duckdb"
    if not exe.exists():
        pytest.skip(f"{exe} not built -- run `make` first")
    return exe


@pytest.fixture
def isolated_home(tmp_path: Path) -> dict:
    """An environment whose HOME is a temp dir: no developer ~/.duckdb leaks in,
    persistent secrets land somewhere disposable, and telemetry is off."""
    env = dict(os.environ)
    env["HOME"] = str(tmp_path)
    env["DATAZOO_DISABLE_TELEMETRY"] = "1"
    return env


@pytest.fixture
def sql(duckdb_cli: Path, isolated_home: dict):
    """Run SQL through the built duckdb shell (one process per call)."""
    def _run(statements: str, expect_error: bool = False, db: str | None = None) -> str:
        args = [str(duckdb_cli), "-noheader", "-list"]
        if db:
            args.append(db)
        # Over stdin, never argv: a CREATE SECRET on the command line is
        # readable by every user of the machine via ps / /proc.
        proc = subprocess.run(args, input=statements, capture_output=True, text=True,
                              timeout=900, env=isolated_home)
        output = proc.stdout + proc.stderr
        if expect_error:
            if proc.returncode == 0:
                raise AssertionError(f"expected an error, got success:\n{scrub(output)}")
        elif proc.returncode != 0:
            raise AssertionError(f"duckdb failed ({proc.returncode}):\n{scrub(output)}")
        return output.strip()

    return _run


class Session:
    """One long-lived duckdb process: state (caches, secrets) survives across
    statements, which is what a stale-cache test needs. Each statement's last
    output line is tagged so the reader knows where its answer ends."""

    def __init__(self, exe: Path, env: dict):
        self.proc = subprocess.Popen([str(exe), "-noheader", "-list"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, env=env)
        self.n = 0

    def query(self, sql: str) -> list[str]:
        self.n += 1
        marker = f"__end_{self.n}__"
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(sql.rstrip().rstrip(";") + f";\nSELECT '{marker}';\n")
        self.proc.stdin.flush()
        lines = []
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line == marker:
                return lines
            lines.append(line)
        raise AssertionError(f"duckdb exited mid-query; output so far: {lines}")

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        self.proc.wait(timeout=120)


@pytest.fixture
def session(duckdb_cli: Path, isolated_home: dict, secret_sql: str):
    s = Session(duckdb_cli, isolated_home)
    s.query(secret_sql)
    try:
        yield s
    finally:
        s.close()
