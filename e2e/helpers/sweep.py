"""Delete stale per-test scratch folders.

A crashed run leaks its `_duckdb_sharepoint_e2e/run-<uuid>` folder (teardown
never ran). Left alone these accumulate in a REAL document library. Run by
`make sweep_orphans`. The cutoff is generous (24 h) so a long local session is
never swept out from under itself. Ported from gdrive's helpers/sweep.py.
"""

from __future__ import annotations

import datetime as dt
import os
import sys

from .graph import Graph, GraphConfigError

SCRATCH_ROOT = "_duckdb_sharepoint_e2e"
DEFAULT_MAX_AGE_HOURS = 24


SITE_SCRATCH_ROOT = "_duckdb_sharepoint_e2e_site"  # test_secret_scoping, on the site's own drive


def site_drive_graph(graph: Graph) -> Graph | None:
    """The second drive the scoping test writes to (SHAREPOINT_SITE_ID's
    default library), or None when not configured."""
    site = os.environ.get("SHAREPOINT_SITE_ID")
    if not site:
        return None
    r = graph._req("GET", f"https://graph.microsoft.com/v1.0/sites/{site}/drive?$select=id")
    r.raise_for_status()
    drive = r.json()["id"]
    return None if drive == graph.drive_id else Graph(graph.tenant, graph.client_id, graph.client_secret, drive)


def sweep(graph: Graph, max_age_hours: int = DEFAULT_MAX_AGE_HOURS, root: str = SCRATCH_ROOT) -> int:
    cutoff = dt.datetime.now(dt.timezone.utc) - dt.timedelta(hours=max_age_hours)
    removed = 0
    for child in graph.children(root):
        if not child["name"].startswith("run-") or "folder" not in child:
            continue
        modified = dt.datetime.fromisoformat(child["lastModifiedDateTime"].replace("Z", "+00:00"))
        if modified < cutoff:
            graph.delete(f"{root}/{child['name']}", permanent=True)
            removed += 1
    print(f"swept {removed} scratch folder(s) under /{root} older than {max_age_hours} h")
    return removed


if __name__ == "__main__":
    try:
        hours = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MAX_AGE_HOURS
        main_graph = Graph.from_env()
        sweep(main_graph, hours)
        other = site_drive_graph(main_graph)
        if other is not None:
            sweep(other, hours, SITE_SCRATCH_ROOT)
    except GraphConfigError as e:
        sys.exit(f"sweep: {e}")
