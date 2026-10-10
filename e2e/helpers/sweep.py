"""Delete stale per-test scratch folders.

A crashed run leaks its `_duckdb_sharepoint_e2e/run-<uuid>` folder (teardown
never ran). Left alone these accumulate in a REAL document library. Run by
`make sweep_orphans`. The cutoff is generous (24 h) so a long local session is
never swept out from under itself. Ported from gdrive's helpers/sweep.py.
"""

from __future__ import annotations

import datetime as dt
import sys

from .graph import Graph, GraphConfigError

SCRATCH_ROOT = "_duckdb_sharepoint_e2e"
DEFAULT_MAX_AGE_HOURS = 24


def sweep(graph: Graph, max_age_hours: int = DEFAULT_MAX_AGE_HOURS) -> int:
    cutoff = dt.datetime.now(dt.timezone.utc) - dt.timedelta(hours=max_age_hours)
    removed = 0
    for child in graph.children(SCRATCH_ROOT):
        if not child["name"].startswith("run-") or "folder" not in child:
            continue
        modified = dt.datetime.fromisoformat(child["lastModifiedDateTime"].replace("Z", "+00:00"))
        if modified < cutoff:
            graph.delete(f"{SCRATCH_ROOT}/{child['name']}", permanent=True)
            removed += 1
    print(f"swept {removed} scratch folder(s) older than {max_age_hours} h")
    return removed


if __name__ == "__main__":
    try:
        sweep(Graph.from_env(), int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MAX_AGE_HOURS)
    except GraphConfigError as e:
        sys.exit(f"sweep: {e}")
