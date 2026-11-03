#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_sharepoint_list(list, site := ...): a SharePoint list as a
//! table through Microsoft Graph, over the same secret as sharepoint://. See
//! docs/hld.md §10.
void RegisterListFunctions(ExtensionLoader &loader);

// --- For an ATTACHed site (#60, sharepoint_catalog.*) -----------------------

//! A list of a site, as an ATTACHed catalog shows it: its id and display name,
//! and its columns, typed from their definitions alone.
struct CatalogList {
	string id;
	string name;
	vector<string> column_names;
	vector<LogicalType> column_types;
};

//! The site's visible lists ("" = the secret's own site), with their columns:
//! one Graph call per page of lists ($expand=columns). Also the site's id.
vector<CatalogList> ListCatalogLists(ClientContext &context, const string &site, string &site_id);

//! read_sharepoint_list's table function (scan, filter and projection
//! pushdown), for a catalog table's GetScanFunction.
TableFunction ListScanFunction();

//! Bind data for scanning list `list_id` of `site_id` with ListScanFunction,
//! its columns typed as ListCatalogLists types them.
unique_ptr<FunctionData> BindListForCatalog(ClientContext &context, const string &list_id, const string &site_id,
                                            vector<string> &names, vector<LogicalType> &types);

//! Writes items of one list in $batch calls of 20: an ATTACHed table's
//! INSERT, UPDATE and DELETE. Columns are matched by name as COPY ... (FORMAT
//! sharepoint_list) matches them (read-only ones skipped).
class ListItemWriter {
public:
	//! `ids_only`: for deletes, no columns to map.
	ListItemWriter(ClientContext &context, const string &list_id, const string &site_id,
	               const vector<string> &column_names, bool ids_only = false);
	~ListItemWriter();
	//! Values aligned with `column_names`; a NULL `id` creates an item.
	void Write(const vector<Value> &values, const Value &id);
	void Delete(const Value &id);
	//! Send what is queued (also every 20 rows on its own).
	void Flush();
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
