#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_excel_table(path, table) and read_excel_range(path, range):
//! live reads of an .xlsx in SharePoint / OneDrive through the Graph workbook
//! API, next to the sharepoint:// filesystem and over the same secret. See
//! docs/hld.md §9.
void RegisterExcelFunctions(ExtensionLoader &loader);

// --- For an ATTACHed site's workbooks (#100, sharepoint_catalog.*) ---------------

//! The names of the Excel tables of the workbook at `path`.
vector<string> ExcelWorkbookTables(ClientContext &context, const string &path);

//! read_excel_table's bind for `table` of `path`: the bind data for
//! ExcelTableScanFunction, and its columns.
unique_ptr<FunctionData> BindExcelTableForCatalog(ClientContext &context, const string &path, const string &table,
                                                  vector<string> &names, vector<LogicalType> &types);
TableFunction ExcelTableScanFunction();

//! Appends rows to a workbook table, as COPY ... (FORMAT excel_table) does:
//! columns matched by name, one workbook session.
class ExcelTableAppender {
public:
	ExcelTableAppender(ClientContext &context, const string &path, const string &table, const vector<string> &names,
	                   const vector<LogicalType> &types);
	~ExcelTableAppender();
	void Append(ExecutionContext &context, DataChunk &chunk);
	void Finish(ClientContext &context);
	idx_t Count() const {
		return written;
	}

private:
	struct State;
	unique_ptr<State> state;
	idx_t written = 0;
};

} // namespace sharepoint
} // namespace duckdb
