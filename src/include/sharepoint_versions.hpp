#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers sharepoint_versions(path): a file's version history, newest
//! first, with a path that reads each version (`...?version=3.0`). See
//! docs/hld.md §11.
void RegisterVersionFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
