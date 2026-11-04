#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers sharepoint_permissions(path) (#98).
void RegisterPermissionFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
