"""Planner and To Do tasks (#68) against the REAL tenant. Needs Graph
permissions the CI app does not have by default, so it skips unless set up:
  SHAREPOINT_PLANNER_PLAN_ID  a plan with at least one task (the app needs
                              Tasks.Read.All, application)
  SHAREPOINT_TODO_USER        a user with a To Do task (the same permission)"""

from __future__ import annotations

import os

import pytest

pytestmark = pytest.mark.live


def test_planner_tasks(graph, sql, secret_sql):
    plan = os.environ.get("SHAREPOINT_PLANNER_PLAN_ID")
    if not plan:
        pytest.skip("SHAREPOINT_PLANNER_PLAN_ID not set")
    out = sql(f"""{secret_sql}
        SELECT count(*) > 0, bool_and(plan_id = '{plan}'), bool_and(status IN ('not started', 'in progress', 'completed')),
               bool_and(bucket IS NOT NULL) FROM read_planner_tasks('{plan}');
    """).splitlines()
    assert out[-1] == "true|true|true|true", out


def test_todo_tasks(graph, sql, secret_sql):
    user = os.environ.get("SHAREPOINT_TODO_USER")
    if not user:
        pytest.skip("SHAREPOINT_TODO_USER not set")
    out = sql(f"{secret_sql} SELECT count(*) > 0, bool_and(list IS NOT NULL) FROM read_todo_tasks(user := '{user}');")
    assert out.splitlines()[-1] == "true|true", out
