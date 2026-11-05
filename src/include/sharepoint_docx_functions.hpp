#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_docx(path) and read_docx_table(path[, n]) (#103).
void RegisterDocxFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
