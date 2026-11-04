#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_onenote_pages(site :=, notebook :=, section :=) (#102).
void RegisterOneNoteFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
