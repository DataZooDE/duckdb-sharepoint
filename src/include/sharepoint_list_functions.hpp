#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_sharepoint_list(list, site := ...): a SharePoint list as a
//! table through Microsoft Graph, over the same secret as sharepoint://. See
//! docs/hld.md §10.
void RegisterListFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
