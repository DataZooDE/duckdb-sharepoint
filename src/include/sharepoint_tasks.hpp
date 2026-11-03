#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_planner_tasks(plan, group := ...) and read_todo_tasks(list :=,
//! user :=): Planner and To Do tasks as tables (#68). See docs/hld.md §18.
void RegisterTaskFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
