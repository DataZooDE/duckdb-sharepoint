#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_excel_table(path, table) and read_excel_range(path, range):
//! live reads of an .xlsx in SharePoint / OneDrive through the Graph workbook
//! API, next to the sharepoint:// filesystem and over the same secret. See
//! docs/hld.md §9.
void RegisterExcelFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
