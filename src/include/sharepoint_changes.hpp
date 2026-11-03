#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers sharepoint_changes(path, token :=): what changed in the drive
//! since a token, from Graph's delta query (#62). See docs/hld.md §15.
void RegisterChangeFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
