"""The community-extensions descriptor's `hello_world` runs, as written, against
a real tenant.

duckdb/community-extensions builds the extension from our tagged commit and
runs no live test of ours; its reviewers read `hello_world` as a claim and try
it. So every statement in it is executed here, with only the placeholders a
reader would replace swapped for real values: the secret's ids (taken from the
environment through getenv(), never inlined), the paths (rooted in a scratch
folder seeded with exactly the files the snippet reads) and the example item id
(the real id of the seeded CSV).
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.live

DESCRIPTOR = Path(__file__).resolve().parents[2] / "docs" / "community-extension-description.yml"


def hello_world() -> str:
    """The `docs.hello_world` literal block, dedented. Parsed by hand: the
    harness has no YAML dependency, and the block is the one shape we need."""
    lines = DESCRIPTOR.read_text().splitlines()
    start = lines.index("  hello_world: |") + 1
    block = []
    for line in lines[start:]:
        if line and not line.startswith("    "):
            break
        block.append(line[4:])
    return "\n".join(block).strip()


def statements(sql: str) -> list[str]:
    """Split on `;` at the end of a line, dropping comment-only lines."""
    body = "\n".join(l for l in sql.splitlines() if not l.lstrip().startswith("--"))
    return [s.strip() for s in re.split(r";\s*$", body, flags=re.M) if s.strip()]


def test_descriptor_ref_is_a_placeholder_or_a_full_sha():
    m = re.search(r"^  ref: (\S+)$", DESCRIPTOR.read_text(), re.M)
    assert m, "descriptor has no repo.ref"
    ref = m.group(1)
    assert ref == "PLACEHOLDER_SET_AT_RELEASE" or re.fullmatch(r"[0-9a-f]{40}", ref), ref


def test_descriptor_variegata_is_the_same_commit_as_ref():
    # One commit builds on DuckDB 2.0 (ref) and v1.5.6 (variegata); a bump that
    # moves only one of them ships two code lines under one version.
    text = DESCRIPTOR.read_text()
    ref = re.search(r"^  ref: (\S+)$", text, re.M)
    variegata = re.search(r"^  variegata: (\S+)$", text, re.M)
    assert ref and variegata, "descriptor needs both repo.ref and repo.variegata"
    assert variegata.group(1) == ref.group(1)


def test_hello_world_runs_against_a_real_tenant(graph, scratch, sql):
    # Seed exactly what the snippet reads.
    seed = "\n".join([
        "COPY (SELECT range AS i FROM range(10)) TO '{p}/Finance/2026/actuals.parquet';",
        "COPY (SELECT 1 AS a) TO '{p}/exports/parts/p1.parquet';",
        "COPY (SELECT 2 AS a) TO '{p}/exports/parts/p2.parquet';",
        "COPY (SELECT 'x' AS c) TO '{p}/by_id.csv' (HEADER);",
    ]).format(p=f"sharepoint://{scratch}")
    secret = ("CREATE OR REPLACE SECRET seed (TYPE sharepoint, PROVIDER client_credentials, "
              "TENANT_ID getenv('SHAREPOINT_TENANT_ID'), CLIENT_ID getenv('SHAREPOINT_CLIENT_ID'), "
              "CLIENT_SECRET getenv('SHAREPOINT_CLIENT_SECRET'), DRIVE_ID getenv('SHAREPOINT_DRIVE_ID'));")
    sql(secret + "\n" + seed)
    item_id = graph.stat(f"{scratch}/by_id.csv")["id"]

    stmts = statements(hello_world())
    assert stmts and stmts[0].startswith("CREATE SECRET"), "hello_world must open with its secret"

    # The reader's substitutions, and nothing else.
    subs = {
        "'00000000-0000-0000-0000-000000000000'": "getenv('SHAREPOINT_TENANT_ID')",
        "'11111111-1111-1111-1111-111111111111'": "getenv('SHAREPOINT_CLIENT_ID')",
        "CLIENT_SECRET '...'": "CLIENT_SECRET getenv('SHAREPOINT_CLIENT_SECRET')",
        "'b!AbCdEf...'": "getenv('SHAREPOINT_DRIVE_ID')",
        "sharepoint://id:01ABCDEF23456789": f"sharepoint://id:{item_id}",
    }
    script = []
    for s in stmts:
        for old, new in subs.items():
            s = s.replace(old, new)
        s = re.sub(r"sharepoint://(?!id:)", f"sharepoint://{scratch}/", s)
        script.append(s + ";")
    # `t` is the reader's own table.
    script.insert(1, "CREATE TABLE t AS SELECT 42 AS answer;")

    out = sql("\n".join(script))
    rows = out.splitlines()
    assert "10" in rows, out              # count(*) over the path-addressed Parquet
    assert "x" in rows, out               # the id:-addressed CSV
    assert {"1", "2"} <= set(rows), out   # the folder glob
    landed = graph.stat(f"{scratch}/reports/out.parquet")
    assert landed is not None and landed["size"] > 0
