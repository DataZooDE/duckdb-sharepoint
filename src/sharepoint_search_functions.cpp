// search_sharepoint() (#67): Microsoft Search (POST /search/query) across the
// tenant's files, list items and sites, as rows. App-only search must name the
// tenant's region; delegated search must not (see SearchRegions).
#include "sharepoint_search_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_search.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <cstdint>
#include <unordered_map>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

//! Hits per request (Graph allows up to 500 for files; smaller pages keep the
//! first results quick).
constexpr int64_t kHitsPerPage = 200;
constexpr int64_t kDefaultMaxResults = 500;

//! The region app-only search worked with, per identity (found once).
mutex g_region_lock;
std::unordered_map<string, string> g_regions;

string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
}

const picojson::object *Obj(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::object>()) ? &it->second.get<picojson::object>() : nullptr;
}

Value Text(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

Value Time(const picojson::object &o, const char *key) {
	int64_t us;
	const auto s = Str(o, key);
	return !s.empty() && ParseIsoTimestamp(s, us) ? Value::TIMESTAMP(timestamp_t(us)) : Value(LogicalType::TIMESTAMP);
}

struct SearchBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

//! One page of results, or throws Graph's error.
picojson::value Query(GraphClient &client, const string &query, const vector<string> &types, int64_t from,
                      int64_t size, const string &region, const string &what) {
	picojson::array entity_types;
	for (auto &t : types) {
		entity_types.emplace_back(t);
	}
	picojson::object q;
	q["queryString"] = picojson::value(query);
	picojson::object request;
	request["entityTypes"] = picojson::value(entity_types);
	request["query"] = picojson::value(q);
	request["from"] = picojson::value(from);
	request["size"] = picojson::value(size);
	if (!region.empty()) {
		request["region"] = picojson::value(region);
	}
	picojson::array requests;
	requests.emplace_back(request);
	picojson::object body;
	body["requests"] = picojson::value(requests);
	picojson::value out;
	const auto text = client.GraphSend("POST", "/v1.0/search/query", picojson::value(body).serialize(), what, true);
	if (!picojson::parse(out, text).empty() || !out.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected response to %s", what);
	}
	return out;
}

//! The first page, finding the region if the search needs one. Returns the
//! region used ("" for none).
picojson::value FirstPage(GraphClient &client, const string &identity, const string &query, const vector<string> &types,
                          int64_t size, string &region, const string &what) {
	if (region.empty()) {
		lock_guard<mutex> guard(g_region_lock);
		auto known = g_regions.find(identity);
		if (known != g_regions.end()) {
			region = known->second;
		}
	}
	if (!region.empty()) {
		return Query(client, query, types, 0, size, region, what);
	}
	try {
		return Query(client, query, types, 0, size, "", what);
	} catch (std::exception &e) {
		if (!SearchNeedsRegion(400, e.what())) {
			throw;
		}
	}
	// App-only: try the regions until the tenant's answers.
	for (auto &candidate : SearchRegions()) {
		try {
			auto page = Query(client, query, types, 0, size, candidate, what);
			region = candidate;
			lock_guard<mutex> guard(g_region_lock);
			g_regions[identity] = candidate;
			return page;
		} catch (std::exception &e) {
			if (!SearchRegionIsWrong(400, e.what())) {
				throw;
			}
		}
	}
	throw InvalidInputException("sharepoint: %s needs the tenant's region and none of the known ones worked; pass "
	                            "region := '<code>' (e.g. 'EUR', 'NAM')",
	                            what);
}

unique_ptr<FunctionData> SearchBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
			throw InvalidInputException("sharepoint: search_sharepoint needs a query string");
		}
		const auto query = StringValue::Get(input.inputs[0]);
		vector<string> types = {"driveItem", "listItem"};
		string region;
		int64_t max_results = kDefaultMaxResults;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "types")) {
			if (!v->IsNull()) {
				types.clear();
				for (auto &t : ListValue::GetChildren(*v)) {
					if (!t.IsNull()) {
						types.push_back(StringValue::Get(t));
					}
				}
			}
		}
		if (types.empty()) {
			throw InvalidInputException("sharepoint: types := needs at least one of 'driveItem', 'listItem', 'site'");
		}
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "region")) {
			if (!v->IsNull()) {
				region = StringValue::Get(*v);
			}
		}
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "max_results")) {
			if (!v->IsNull()) {
				max_results = v->GetValue<int64_t>();
			}
		}
		if (max_results <= 0) {
			throw InvalidInputException("sharepoint: max_results must be positive");
		}

		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, SHAREPOINT_SCHEME);
		GraphClient client(auth);
		const string what = "the search for '" + query + "'";
		auto data = make_uniq<SearchBindData>();
		const auto size = std::min<int64_t>(kHitsPerPage, max_results);
		auto page = FirstPage(client, auth.identity, query, types, size, region, what);
		for (int64_t from = 0;;) {
			bool more = false;
			int64_t hits_in_page = 0;
			auto values = page.get<picojson::object>().find("value");
			if (values != page.get<picojson::object>().end() && values->second.is<picojson::array>()) {
				for (auto &response : values->second.get<picojson::array>()) {
					if (!response.is<picojson::object>()) {
						continue;
					}
					auto containers = response.get<picojson::object>().find("hitsContainers");
					if (containers == response.get<picojson::object>().end() ||
					    !containers->second.is<picojson::array>()) {
						continue;
					}
					for (auto &container : containers->second.get<picojson::array>()) {
						if (!container.is<picojson::object>()) {
							continue;
						}
						const auto &c = container.get<picojson::object>();
						auto more_it = c.find("moreResultsAvailable");
						more = more || (more_it != c.end() && more_it->second.is<bool>() && more_it->second.get<bool>());
						auto hits = c.find("hits");
						if (hits == c.end() || !hits->second.is<picojson::array>()) {
							continue;
						}
						for (auto &hit_v : hits->second.get<picojson::array>()) {
							if (!hit_v.is<picojson::object>() ||
							    static_cast<int64_t>(data->rows.size()) >= max_results) {
								continue;
							}
							hits_in_page++;
							const auto &hit = hit_v.get<picojson::object>();
							const auto *res = Obj(hit, "resource");
							if (!res) {
								continue;
							}
							auto type = Str(*res, "@odata.type");
							type = type.substr(type.rfind('.') == string::npos ? 0 : type.rfind('.') + 1);
							string drive_id, site_id, list_id, parent_path;
							if (auto parent = Obj(*res, "parentReference")) {
								drive_id = Str(*parent, "driveId");
								site_id = Str(*parent, "siteId");
								parent_path = Str(*parent, "path");
								if (auto ids = Obj(*parent, "sharepointIds")) {
									list_id = Str(*ids, "listId");
								}
							}
							if (auto ids = Obj(*res, "sharepointIds")) {
								list_id = list_id.empty() ? Str(*ids, "listId") : list_id;
								site_id = site_id.empty() ? Str(*ids, "siteId") : site_id;
							}
							auto name = Str(*res, "name");
							if (name.empty()) {
								name = Str(*res, "displayName");
							}
							// A file in the secret's drive can be read back by path
							// (or, without a parent path, by id).
							string path;
							const auto id = Str(*res, "id");
							if (type == "driveItem" && !drive_id.empty() && drive_id == auth.drive_id) {
								const auto rel = SearchHitPath(parent_path, name);
								path = string(SHAREPOINT_SCHEME) + (rel.empty() ? "id:" + id : rel);
							}
							Value modified_by(LogicalType::VARCHAR);
							if (auto by = Obj(*res, "lastModifiedBy")) {
								if (auto user = Obj(*by, "user")) {
									modified_by = Text(Str(*user, "displayName"));
								}
							}
							auto size_it = res->find("size");
							auto rank_it = hit.find("rank");
							data->rows.push_back(
							    {Text(type), Text(name), Text(path), Text(id), Text(Str(*res, "webUrl")), Text(drive_id),
							     Text(site_id), Text(list_id),
							     size_it != res->end() && size_it->second.is<int64_t>()
							         ? Value::BIGINT(size_it->second.get<int64_t>())
							         : Value(LogicalType::BIGINT),
							     Time(*res, "createdDateTime"), Time(*res, "lastModifiedDateTime"), modified_by,
							     Text(Str(hit, "summary")),
							     rank_it != hit.end() && rank_it->second.is<int64_t>()
							         ? Value::INTEGER(static_cast<int32_t>(rank_it->second.get<int64_t>()))
							         : Value(LogicalType::INTEGER)});
						}
					}
				}
			}
			from += size;
			if (!more || hits_in_page == 0 || static_cast<int64_t>(data->rows.size()) >= max_results) {
				break;
			}
			page = Query(client, query, types, from, size, region, what);
		}
		names = {"type",    "name",     "path",    "id",       "web_url",     "drive_id",
		         "site_id", "list_id",  "size",    "created",  "modified",    "modified_by",
		         "summary", "rank"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::BIGINT,  LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::INTEGER};
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

void SearchScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<SearchBindData>();
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

} // namespace

void RegisterSearchFunctions(ExtensionLoader &loader) {
	TableFunction fn("search_sharepoint", {LogicalType::VARCHAR}, SearchScan, SearchBind, CursorInit);
	duckdb_compat::NameParameters(fn, {"query"});
	duckdb_compat::AddNamedParameter(fn, "types", LogicalType::LIST(LogicalType::VARCHAR));
	duckdb_compat::AddNamedParameter(fn, "region", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "max_results", LogicalType::BIGINT);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "Microsoft Search across the tenant's files, list items and sites (types := ['driveItem', 'listItem', "
	    "'site']), as rows: type, name, a sharepoint:// path for a file in the secret's drive, id, web URL, drive, "
	    "site and list ids, size, times, who modified it, the hit summary and its rank. App-only search finds the "
	    "tenant's region itself, or takes region := 'EUR'.";
	desc.examples = {"SELECT name, path, modified FROM search_sharepoint('quarterly forecast')",
	                 "SELECT name, web_url FROM search_sharepoint('Sales', types := ['site'])"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
