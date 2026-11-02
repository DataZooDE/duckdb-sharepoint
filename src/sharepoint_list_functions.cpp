// read_sharepoint_list(): a SharePoint list as a table -- one row per item,
// one typed column per list column -- through Microsoft Graph, over the same
// secret as sharepoint://. A table function beside the filesystem; it changes
// nothing about it. The column plan is pure (sharepoint_list.*); the Graph
// shapes are recorded in docs/hld.md §10.
#include "sharepoint_list_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <cstdint>
#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

//! Items per page by default (sharepoint_list_page_size). Graph pages list
//! items with @odata.nextLink; paging alone works past the 5,000-item list
//! view threshold (no filter, no order).
constexpr int64_t kItemsPerPage = 999;

int64_t ListPageSize(ClientContext &context) {
	Value v;
	auto opener = ClientData::Get(context).file_opener.get();
	if (duckdb_compat::TryGetSetting(opener, "sharepoint_list_page_size", v) && !v.IsNull()) {
		return v.GetValue<int64_t>();
	}
	return kItemsPerPage;
}
//! A nextLink chain longer than this is a server bug, not a list.
constexpr int kMaxItemPages = 100000;

const picojson::object &AsObject(const string &body, const string &what, picojson::value &holder) {
	if (!picojson::parse(holder, body).empty() || !holder.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected response reading %s", what);
	}
	return holder.get<picojson::object>();
}

string StringField(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
}

bool BoolField(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<bool>() && it->second.get<bool>();
}

ListFacet FacetOf(const string &name) {
	if (name == "text") {
		return ListFacet::TEXT;
	}
	if (name == "number") {
		return ListFacet::NUMBER;
	}
	if (name == "currency") {
		return ListFacet::CURRENCY;
	}
	if (name == "boolean") {
		return ListFacet::BOOLEAN;
	}
	if (name == "dateTime") {
		return ListFacet::DATETIME;
	}
	if (name == "choice") {
		return ListFacet::CHOICE;
	}
	if (name == "lookup") {
		return ListFacet::LOOKUP;
	}
	if (name == "personOrGroup") {
		return ListFacet::PERSON;
	}
	if (name == "hyperlinkOrPicture") {
		return ListFacet::HYPERLINK;
	}
	if (name == "calculated") {
		return ListFacet::CALCULATED;
	}
	return ListFacet::OTHER;
}

ListColumnDef ParseColumnDef(const picojson::object &o) {
	ListColumnDef def;
	def.name = StringField(o, "name");
	def.display_name = StringField(o, "displayName");
	def.hidden = BoolField(o, "hidden");
	def.read_only = BoolField(o, "readOnly");
	// The facet is the one object-valued key naming a kind.
	for (const char *facet : {"text", "number", "currency", "boolean", "dateTime", "choice", "lookup", "personOrGroup",
	                          "hyperlinkOrPicture", "calculated"}) {
		auto it = o.find(facet);
		if (it != o.end() && it->second.is<picojson::object>()) {
			def.facet = FacetOf(facet);
			if (def.facet == ListFacet::CALCULATED) {
				auto output = StringField(it->second.get<picojson::object>(), "outputType");
				def.calculated_output = output == "number"     ? ListFacet::NUMBER
				                        : output == "boolean"  ? ListFacet::BOOLEAN
				                        : output == "dateTime" ? ListFacet::DATETIME
				                                               : ListFacet::TEXT;
			}
			break;
		}
	}
	return def;
}

LogicalType ToLogicalType(ListColumnType t) {
	switch (t) {
	case ListColumnType::VARCHAR_LIST:
		return LogicalType::LIST(LogicalType::VARCHAR);
	case ListColumnType::DOUBLE:
		return LogicalType::DOUBLE;
	case ListColumnType::DECIMAL:
		return LogicalType::DECIMAL(18, 4);
	case ListColumnType::BOOLEAN:
		return LogicalType::BOOLEAN;
	case ListColumnType::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case ListColumnType::BIGINT:
		return LogicalType::BIGINT;
	default:
		return LogicalType::VARCHAR;
	}
}

//! A field's text: strings as-is, a hyperlink's URL, anything else as JSON.
string FieldText(const picojson::value &v) {
	if (v.is<string>()) {
		return v.get<string>();
	}
	if (v.is<picojson::object>()) {
		auto url = StringField(v.get<picojson::object>(), "Url");
		if (!url.empty()) {
			return url;
		}
	}
	if (v.is<bool>()) {
		return v.get<bool>() ? "true" : "false";
	}
	return v.serialize();
}

Value ToValue(const picojson::value *v, ListColumnType t) {
	const auto type = ToLogicalType(t);
	if (!v || v->is<picojson::null>()) {
		return Value(type);
	}
	switch (t) {
	case ListColumnType::BIGINT:
		if (v->is<int64_t>()) {
			return Value::BIGINT(v->get<int64_t>());
		}
		if (v->is<string>()) {
			int64_t n;
			if (TryCast::Operation<string_t, int64_t>(string_t(v->get<string>()), n, false)) {
				return Value::BIGINT(n);
			}
		}
		return Value(type);
	case ListColumnType::DOUBLE:
	case ListColumnType::DECIMAL: {
		double d;
		if (v->is<int64_t>()) {
			d = static_cast<double>(v->get<int64_t>());
		} else if (v->is<double>()) {
			d = v->get<double>();
		} else if (!v->is<string>() || !TryCast::Operation<string_t, double>(string_t(v->get<string>()), d, false)) {
			return Value(type);
		}
		return t == ListColumnType::DOUBLE ? Value::DOUBLE(d) : Value::DOUBLE(d).DefaultCastAs(type);
	}
	case ListColumnType::BOOLEAN:
		return v->is<bool>() ? Value::BOOLEAN(v->get<bool>()) : Value(type);
	case ListColumnType::TIMESTAMP: {
		int64_t us;
		if (v->is<string>() && ParseIsoTimestamp(v->get<string>(), us)) {
			return Value::TIMESTAMP(timestamp_t(us));
		}
		return Value(type);
	}
	case ListColumnType::VARCHAR_LIST: {
		vector<Value> items;
		if (v->is<picojson::array>()) {
			for (auto &e : v->get<picojson::array>()) {
				items.emplace_back(FieldText(e));
			}
		} else {
			items.emplace_back(FieldText(*v));
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(items));
	}
	default:
		return Value(FieldText(*v));
	}
}

struct ListBindData : public TableFunctionData {
	vector<ListColumnType> types;
	vector<vector<Value>> rows;
};

//! The Graph site id ("host,siteGuid,webGuid") for `site`: the secret's own
//! site (that of its drive) when empty; else an id as given, a server-relative
//! path on the same host ("Sales", "sites/Sales", "teams/Ops"), or
//! "host:/sites/Sales".
string ResolveSite(GraphClient &client, const AuthContext &auth, const string &site) {
	if (site.find(',') != string::npos) {
		return site;
	}
	picojson::value holder;
	const auto &library = AsObject(
	    client.GraphGet("/v1.0/drives/" + auth.drive_id + "/list?$select=parentReference", "the secret's site"),
	    "the secret's site", holder);
	string own_site;
	auto parent = library.find("parentReference");
	if (parent != library.end() && parent->second.is<picojson::object>()) {
		own_site = StringField(parent->second.get<picojson::object>(), "siteId");
	}
	if (own_site.empty()) {
		throw IOException("sharepoint: could not tell which site the secret's drive belongs to");
	}
	if (site.empty()) {
		return own_site;
	}
	string address = site;
	if (site.find(':') == string::npos) {
		// Same host as the secret's site; a bare name means /sites/<name>.
		string relative = site;
		while (!relative.empty() && relative[0] == '/') {
			relative.erase(0, 1);
		}
		if (relative.find('/') == string::npos) {
			relative = "sites/" + relative;
		}
		address = own_site.substr(0, own_site.find(',')) + ":/" + relative;
	}
	const string what = "site '" + site + "'";
	picojson::value site_holder;
	const auto &found = AsObject(client.GraphGet("/v1.0/sites/" + address + "?$select=id", what), what, site_holder);
	auto id = StringField(found, "id");
	if (id.empty()) {
		throw IOException("sharepoint: %s has no id", what);
	}
	return id;
}

unique_ptr<FunctionData> ListBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the list name must not be NULL");
		}
		const auto list = StringValue::Get(input.inputs[0]);
		if (list.empty() || list == "id:") {
			throw InvalidInputException("sharepoint: name a list, by its name or as id:<listId>");
		}
		string site;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "site")) {
			if (!v->IsNull()) {
				site = StringValue::Get(*v);
			}
		}
		// Lists hang off a site, not a path: the secret in scope for the
		// drive root is the one used.
		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, SHAREPOINT_SCHEME);
		GraphClient client(auth);
		const auto site_id = ResolveSite(client, auth, site);

		const bool by_id = StringUtil::StartsWith(list, "id:");
		const string what = "list '" + list + "'" + (site.empty() ? string() : " on site '" + site + "'");
		const string list_ref = by_id ? EncodeWorkbookName(list.substr(3)) : EncodeWorkbookName(list);
		picojson::value holder;
		const auto &meta =
		    AsObject(client.GraphGet("/v1.0/sites/" + site_id + "/lists/" + list_ref + "?$select=id,displayName", what),
		             what, holder);
		const string base = "/v1.0/sites/" + site_id + "/lists/" + StringField(meta, "id");

		vector<ListColumnDef> defs;
		picojson::value cols_holder;
		const auto &cols = AsObject(client.GraphGet(base + "/columns", what), what, cols_holder);
		auto values = cols.find("value");
		if (values != cols.end() && values->second.is<picojson::array>()) {
			for (auto &c : values->second.get<picojson::array>()) {
				if (c.is<picojson::object>()) {
					defs.push_back(ParseColumnDef(c.get<picojson::object>()));
				}
			}
		}
		auto plan = PlanListColumns(defs);

		// Every item first: whether a choice column is multi-valued shows only
		// in the data.
		vector<picojson::object> items;
		// Name every planned field: a lookup's display value is only returned
		// when it is selected explicitly (by default Graph sends <name>LookupId
		// alone -- found live).
		string select;
		for (auto &col : plan) {
			select += (select.empty() ? "" : ",") + EncodeWorkbookName(col.field);
		}
		string next = base + "/items?$expand=fields($select=" + select + ")&$top=" +
		              std::to_string(ListPageSize(context));
		for (int pages = 0; !next.empty(); pages++) {
			if (pages >= kMaxItemPages) {
				throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxItemPages);
			}
			picojson::value page_holder;
			const auto &page = AsObject(client.GraphGet(next, what), what, page_holder);
			auto page_items = page.find("value");
			if (page_items != page.end() && page_items->second.is<picojson::array>()) {
				for (auto &item : page_items->second.get<picojson::array>()) {
					if (!item.is<picojson::object>()) {
						continue;
					}
					auto fields = item.get<picojson::object>().find("fields");
					items.push_back(fields != item.get<picojson::object>().end() &&
					                        fields->second.is<picojson::object>()
					                    ? fields->second.get<picojson::object>()
					                    : picojson::object());
				}
			}
			auto link = page.find("@odata.nextLink");
			next = (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>())
			                                                         : string();
		}
		for (auto &col : plan) {
			if (!col.may_be_multi) {
				continue;
			}
			for (auto &fields : items) {
				auto it = fields.find(col.field);
				if (it != fields.end() && it->second.is<picojson::array>()) {
					col.type = ListColumnType::VARCHAR_LIST;
					break;
				}
			}
		}

		auto data = make_uniq<ListBindData>();
		for (auto &col : plan) {
			data->types.push_back(col.type);
			return_types.push_back(ToLogicalType(col.type));
			names.emplace_back(col.name);
		}
		data->rows.reserve(items.size());
		for (auto &fields : items) {
			vector<Value> row;
			row.reserve(plan.size());
			for (auto &col : plan) {
				auto it = fields.find(col.field);
				row.push_back(ToValue(it == fields.end() ? nullptr : &it->second, col.type));
			}
			data->rows.push_back(std::move(row));
		}
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

void ListScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ListBindData>();
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

void RegisterListFunctions(ExtensionLoader &loader) {
	TableFunction fn("read_sharepoint_list", {LogicalType::VARCHAR}, ListScan, ListBind, CursorInit);
	duckdb_compat::NameParameters(fn, {"list"});
	duckdb_compat::AddNamedParameter(fn, "site", LogicalType::VARCHAR);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "Read a SharePoint list as a table through Microsoft Graph: one row per item, one typed column per list "
	    "column (named by its display name), plus id, created and modified. The list is named by its name or as "
	    "id:<listId>; it is on the secret's site unless site := names another ('Sales', 'teams/Ops', or a Graph "
	    "site id). Lookup and person columns give their item id as <name>_id.";
	desc.examples = {"SELECT * FROM read_sharepoint_list('Orders')",
	                 "SELECT * FROM read_sharepoint_list('Orders', site := 'Sales')"};
	desc.categories = {"sharepoint", "list"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
