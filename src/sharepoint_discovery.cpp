// Finding the ids nobody knows (#95), over the same secret as the rest:
//   sharepoint_sites(search := ...)  -- GET /sites?search=...
//   sharepoint_drives(site)          -- GET /sites/{id}/drives
// Both work on a secret that names no drive yet: they are how one finds it.
#include "sharepoint_discovery.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list_functions.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_weburl.hpp"

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

AuthContext AnyDriveAuth(ClientContext &context) {
	auto opener = ClientData::Get(context).file_opener.get();
	return GetAuthContext(opener, SHAREPOINT_SCHEME, false);
}

unique_ptr<FunctionData> SitesBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		string search = "*";
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "search")) {
			if (!v->IsNull() && !StringValue::Get(*v).empty()) {
				search = StringValue::Get(*v);
			}
		}
		auto auth = AnyDriveAuth(context);
		GraphClient client(auth);
		auto data = make_uniq<RowsBindData>();
		for (auto &site : All(client,
		                      "/v1.0/sites?search=" + PercentEncodeSegment(search) +
		                          "&$select=id,name,displayName,webUrl,description",
		                      "the sites matching '" + search + "'")) {
			data->rows.push_back({Text(Str(site, "displayName")), Text(Str(site, "webUrl")), Text(Str(site, "name")),
			                      Text(Str(site, "description")), Text(Str(site, "id"))});
		}
		names = {"name", "web_url", "url_name", "description", "id"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR};
		result = std::move(data);
	});
	return result;
}

unique_ptr<FunctionData> DrivesBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		string site;
		if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
			site = StringValue::Get(input.inputs[0]);
		}
		auto auth = AnyDriveAuth(context);
		GraphClient client(auth);
		const auto site_id = ResolveSiteId(client, auth, site);
		auto data = make_uniq<RowsBindData>();
		const string what = "the document libraries of " + (site.empty() ? string("the secret's site") : "'" + site + "'");
		for (auto &drive : All(client, "/v1.0/sites/" + site_id + "/drives?$select=id,name,webUrl,driveType,description",
		                       what)) {
			data->rows.push_back({Text(Str(drive, "name")), Text(Str(drive, "webUrl")), Text(Str(drive, "id")),
			                      Text(Str(drive, "driveType")), Text(Str(drive, "description")), Value(site_id)});
		}
		names = {"name", "web_url", "id", "drive_type", "description", "site_id"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
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

void RegisterDiscoveryFunctions(ExtensionLoader &loader) {
	TableFunction sites("sharepoint_sites", {}, RowsScan, SitesBind, CursorInit);
	duckdb_compat::AddNamedParameter(sites, "search", LogicalType::VARCHAR);
	Register(loader, sites,
	         "The SharePoint sites the credentials can see (search := filters by words in the name): display name, "
	         "web address (use it as SITE_URL or in a path), URL name, description and Graph id. Works before the "
	         "secret names a drive.",
	         {"SELECT name, web_url FROM sharepoint_sites()", "SELECT * FROM sharepoint_sites(search := 'Sales')"});

	// sharepoint_drives() -- the secret's site; sharepoint_drives(site).
	TableFunction drives_of_secret("sharepoint_drives", {}, RowsScan, DrivesBind, CursorInit);
	TableFunction drives("sharepoint_drives", {LogicalType::VARCHAR}, RowsScan, DrivesBind, CursorInit);
	duckdb_compat::NameParameters(drives, {"site"});
	TableFunctionSet set("sharepoint_drives");
	set.AddFunction(drives_of_secret);
	set.AddFunction(drives);
	CreateTableFunctionInfo info(set);
	FunctionDescription desc;
	desc.description = "The document libraries of a site (its browser address, 'Sales', 'sites/Sales' or a Graph "
	                   "id; default the secret's site): name (use it as DRIVE), web address, drive id (DRIVE_ID), "
	                   "type and description.";
	desc.examples = {"SELECT name, web_url, id FROM sharepoint_drives('https://contoso.sharepoint.com/sites/Sales')"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
