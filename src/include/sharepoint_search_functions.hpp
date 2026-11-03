#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers search_sharepoint(query, types :=, region :=, max_results :=):
//! Microsoft Search across the tenant as rows (#67). See docs/hld.md §14.
void RegisterSearchFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
