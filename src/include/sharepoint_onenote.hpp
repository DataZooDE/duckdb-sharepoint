#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_onenote_pages(site :=, notebook :=, section :=, content :=)
//! (#102) and COPY ... (FORMAT onenote) (#105).
void RegisterOneNoteFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
