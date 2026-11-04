// read_onenote_pages() (#102): the pages of a site's OneNote notebooks, with
// their section and change times -- page-level tracking that the filesystem
// (a section is one .one file) cannot give. OneNote has no delta: track with
// WHERE modified > <last run>. Microsoft restricts app-only OneNote access, so
// this is for a signed-in user (Notes.Read / Notes.Read.All).
#include "sharepoint_onenote.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list_functions.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_weburl.hpp"
#include "sharepoint_list.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

constexpr int kMaxPages = 1000;

string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
}

Value Text(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

//! Every object of a paged Graph collection.
vector<picojson::object> All(GraphClient &client, string next, const string &what) {
	vector<picojson::object> out;
	for (int pages = 0; !next.empty(); pages++) {
		if (pages >= kMaxPages) {
			throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxPages);
		}
		picojson::value holder;
		if (!picojson::parse(holder, client.GraphGet(next, what)).empty() || !holder.is<picojson::object>()) {
			throw IOException("sharepoint: unexpected response reading %s", what);
		}
		const auto &page = holder.get<picojson::object>();
		auto values = page.find("value");
		if (values != page.end() && values->second.is<picojson::array>()) {
			for (auto &v : values->second.get<picojson::array>()) {
				if (v.is<picojson::object>()) {
					out.push_back(v.get<picojson::object>());
				}
			}
		}
		auto link = page.find("@odata.nextLink");
		next = (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>())
		                                                         : string();
	}
	return out;
}

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

string Param(TableFunctionBindInput &input, const char *name) {
	if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, name)) {
		if (!v->IsNull()) {
			return StringValue::Get(*v);
		}
	}
	return string();
}

//! A OneNote call: on 401/403 say what usually causes it.
vector<picojson::object> OneNote(GraphClient &client, const string &path, const string &what) {
	try {
		return All(client, path, what);
	} catch (std::exception &e) {
		const string text = e.what();
		if (text.find("HTTP 401") != string::npos || text.find("HTTP 403") != string::npos ||
		    text.find("forbidden") != string::npos) {
			throw InvalidInputException("sharepoint: reading %s was refused. Microsoft restricts app-only access to "
			                            "OneNote: use a signed-in user (PROVIDER device_code or authorization_code) "
			                            "with Notes.Read in GRAPH_SCOPE, e.g. GRAPH_SCOPE 'https://graph.microsoft.com/"
			                            "Notes.Read Files.ReadWrite.All Sites.ReadWrite.All offline_access'. (%s)",
			                            what, text);
		}
		throw;
	}
}

unique_ptr<FunctionData> PagesBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto site = Param(input, "site");
		const auto notebook = Param(input, "notebook");
		const auto section = Param(input, "section");
		auto opener = ClientData::Get(context).file_opener.get();
		auto auth = GetAuthContext(opener, SHAREPOINT_SCHEME, site.empty());
		GraphClient client(auth);
		const auto site_id = ResolveSiteId(client, auth, site);
		const string base = "/v1.0/sites/" + site_id + "/onenote";
		auto data = make_uniq<RowsBindData>();
		bool notebook_found = notebook.empty();
		for (auto &nb : OneNote(client, base + "/notebooks?$select=id,displayName", "the site's OneNote notebooks")) {
			const auto nb_name = Str(nb, "displayName");
			if (!notebook.empty() && !StringUtil::CIEquals(nb_name, notebook)) {
				continue;
			}
			notebook_found = true;
			for (auto &sec : OneNote(client, base + "/notebooks/" + Str(nb, "id") + "/sections?$select=id,displayName",
			                         "the sections of notebook '" + nb_name + "'")) {
				const auto sec_name = Str(sec, "displayName");
				if (!section.empty() && !StringUtil::CIEquals(sec_name, section)) {
					continue;
				}
				for (auto &page : OneNote(client,
				                          base + "/sections/" + Str(sec, "id") +
				                              "/pages?$select=id,title,createdDateTime,lastModifiedDateTime,links&$top=100",
				                          "the pages of section '" + sec_name + "'")) {
					string web_url;
					auto links = page.find("links");
					if (links != page.end() && links->second.is<picojson::object>()) {
						auto web = links->second.get<picojson::object>().find("oneNoteWebUrl");
						if (web != links->second.get<picojson::object>().end() && web->second.is<picojson::object>()) {
							web_url = Str(web->second.get<picojson::object>(), "href");
						}
					}
					int64_t created, modified;
					data->rows.push_back(
					    {Value(nb_name), Value(sec_name), Text(Str(page, "title")),
					     ParseIsoTimestamp(Str(page, "createdDateTime"), created) ? Value::TIMESTAMP(timestamp_t(created))
					                                                              : Value(LogicalType::TIMESTAMP),
					     ParseIsoTimestamp(Str(page, "lastModifiedDateTime"), modified)
					         ? Value::TIMESTAMP(timestamp_t(modified))
					         : Value(LogicalType::TIMESTAMP),
					     Text(web_url), Text(Str(page, "id"))});
				}
			}
		}
		if (!notebook_found) {
			throw InvalidInputException("sharepoint: the site has no OneNote notebook '%s'", notebook);
		}
		names = {"notebook", "section", "title", "created", "modified", "web_url", "id"};
		return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP,
		                LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR};
		result = std::move(data);
	});
	return result;
}

struct CursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> CursorInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CursorState>();
}

void RowsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RowsBindData>();
	auto &state = input.global_state->Cast<CursorState>();
	idx_t out = 0;
	while (state.cursor < data.rows.size() && out < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.cursor];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, out, row[c]);
		}
		state.cursor++;
		out++;
	}
	duckdb_compat::SetChildCardinality(output, out);
}

void Register(ExtensionLoader &loader, TableFunction fn, const string &description, vector<string> examples) {
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterOneNoteFunctions(ExtensionLoader &loader) {
	TableFunction fn("read_onenote_pages", {}, RowsScan, PagesBind, CursorInit);
	duckdb_compat::AddNamedParameter(fn, "site", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "notebook", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "section", LogicalType::VARCHAR);
	Register(loader, fn,
	         "The pages of a site's OneNote notebooks (default the secret's site; notebook := and section := narrow "
	         "it): notebook, section, title, created and modified times, web address and id. OneNote has no delta: "
	         "track changes with WHERE modified > <last run>. Needs a signed-in user with Notes.Read (Microsoft "
	         "restricts app-only OneNote access).",
	         {"SELECT section, title, modified FROM read_onenote_pages(notebook := 'Projects') ORDER BY modified DESC"});
}

} // namespace sharepoint
} // namespace duckdb
