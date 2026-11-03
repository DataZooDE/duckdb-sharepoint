// read_sharepoint_list(): a SharePoint list as a table -- one row per item,
// one typed column per list column -- through Microsoft Graph, over the same
// secret as sharepoint://. A table function beside the filesystem; it changes
// nothing about it. The column plan is pure (sharepoint_list.*); the Graph
// shapes are recorded in docs/hld.md §10.
#include "sharepoint_list_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_copy_rewrite.hpp"
#include "sharepoint_counters.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <algorithm>
#include <cmath>
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

//! A list resolved for reading or writing: where it is and its columns.
struct ListTarget {
	AuthContext auth;
	string site_id;
	string base; // "/v1.0/sites/{site}/lists/{listId}"
	string what; // "list 'Orders'" for messages
	vector<ListOutputColumn> plan;
};

ListTarget ResolveList(ClientContext &context, const string &list, const string &site) {
	if (list.empty() || list == "id:") {
		throw InvalidInputException("sharepoint: name a list, by its name or as id:<listId>");
	}
	ListTarget t;
	// Lists hang off a site, not a path: the secret in scope for the drive
	// root is the one used.
	auto opener = ClientData::Get(context).file_opener.get();
	t.auth = GetAuthContext(opener, SHAREPOINT_SCHEME);
	GraphClient client(t.auth);
	t.site_id = ResolveSite(client, t.auth, site);

	const bool by_id = StringUtil::StartsWith(list, "id:");
	t.what = "list '" + list + "'" + (site.empty() ? string() : " on site '" + site + "'");
	const string list_ref = by_id ? EncodeWorkbookName(list.substr(3)) : EncodeWorkbookName(list);
	picojson::value holder;
	const auto &meta =
	    AsObject(client.GraphGet("/v1.0/sites/" + t.site_id + "/lists/" + list_ref + "?$select=id,displayName", t.what),
	             t.what, holder);
	t.base = "/v1.0/sites/" + t.site_id + "/lists/" + StringField(meta, "id");

	vector<ListColumnDef> defs;
	picojson::value cols_holder;
	const auto &cols = AsObject(client.GraphGet(t.base + "/columns", t.what), t.what, cols_holder);
	auto values = cols.find("value");
	if (values != cols.end() && values->second.is<picojson::array>()) {
		for (auto &c : values->second.get<picojson::array>()) {
			if (c.is<picojson::object>()) {
				defs.push_back(ParseColumnDef(c.get<picojson::object>()));
			}
		}
	}
	t.plan = PlanListColumns(defs);
	return t;
}

unique_ptr<FunctionData> ListBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the list name must not be NULL");
		}
		string site;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "site")) {
			if (!v->IsNull()) {
				site = StringValue::Get(*v);
			}
		}
		auto target = ResolveList(context, StringValue::Get(input.inputs[0]), site);
		GraphClient client(target.auth);
		auto &plan = target.plan;
		const auto &base = target.base;
		const auto &what = target.what;

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

// ===========================================================================
// COPY (SELECT ...) TO 'Orders' (FORMAT sharepoint_list [, SITE 'Sales'])
//
// A row with an `id` updates that item (PATCH items/{id}/fields); a row
// without one (no id column, or a NULL id) creates an item (POST items).
// Columns are matched to the list's by name (PlanListWrite), so a
// read_sharepoint_list result can be changed and written back as it is.
// The target is a list, not a file: the same in-place rewrite as the Excel
// writers keeps COPY from renaming anything (sharepoint_copy_rewrite.hpp).
// Rows go out in Graph $batch calls of 20 requests.
// ===========================================================================

constexpr const char *kListFormat = "sharepoint_list";
constexpr const char *kListFormatInternal = "__sharepoint_list";
constexpr idx_t kBatchRequests = 20; // Graph's $batch limit
constexpr int kMaxBatchAttempts = 6;

struct ListWriteBindData : public FunctionData {
	string list;
	string site;
	vector<string> names;
	vector<LogicalType> types;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ListWriteBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ListWriteBindData>();
		return list == other.list && site == other.site && names == other.names;
	}
};

unique_ptr<FunctionData> ListWriteBind(ClientContext &, CopyFunctionBindInput &input,
                                       const duckdb_compat::CopyBindNames &names,
                                       const vector<LogicalType> &sql_types) {
	auto &info = input.info;
	if (!IsRewrittenInPlace(info)) {
		throw InvalidInputException("sharepoint: write lists with FORMAT %s", kListFormat);
	}
	auto data = make_uniq<ListWriteBindData>();
	data->list = info.file_path;
	if (auto site = FindCopyOption(info.options, "site")) {
		if (site->size() == 1 && !(*site)[0].IsNull()) {
			data->site = (*site)[0].ToString();
		}
	}
	for (idx_t i = 0; i < sql_types.size(); i++) {
		data->names.push_back(duckdb_compat::Str(names[i]));
		data->types.push_back(sql_types[i]);
	}
	return std::move(data);
}

struct PendingRequest {
	string item_id; // empty: create
	picojson::object fields;
};

struct ListWriteGlobalState : public GlobalFunctionData {
	ListTarget target;
	vector<ListWriteColumn> columns;
	vector<PendingRequest> pending;
};

picojson::value ToJsonField(const Value &v, const string &field) {
	if (v.IsNull()) {
		return picojson::value(); // null clears the field
	}
	const auto &type = v.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return picojson::value(BooleanValue::Get(v));
	case LogicalTypeId::DATE: {
		const auto date = v.GetValue<date_t>();
		return picojson::value(FormatIsoTimestamp(int64_t(date.days) * Interval::MICROS_PER_DAY));
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
		return picojson::value(FormatIsoTimestamp(
		    Timestamp::GetEpochMicroSeconds(v.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>())));
	case LogicalTypeId::LIST: {
		picojson::array items;
		for (auto &child : ListValue::GetChildren(v)) {
			items.emplace_back(child.IsNull() ? string() : child.ToString());
		}
		return picojson::value(items);
	}
	default:
		break;
	}
	if (type.IsNumeric()) {
		// A lookup is set by the looked-up item's id, which Graph takes as a string.
		if (StringUtil::EndsWith(field, "LookupId")) {
			return picojson::value(v.DefaultCastAs(LogicalType::BIGINT).ToString());
		}
		const double d = v.GetValue<double>();
		return std::isfinite(d) ? picojson::value(d) : picojson::value();
	}
	return picojson::value(v.ToString());
}

unique_ptr<GlobalFunctionData> ListWriteInitGlobal(ClientContext &context, FunctionData &bind_p, const string &) {
	auto &bind = bind_p.Cast<ListWriteBindData>();
	auto g = make_uniq<ListWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		g->target = ResolveList(context, bind.list, bind.site);
		string error;
		if (!PlanListWrite(bind.names, g->target.plan, g->columns, error)) {
			throw InvalidInputException("sharepoint: %s: %s", g->target.what, error);
		}
	});
	return std::move(g);
}

//! Send the pending requests in one $batch, re-sending only the parts Graph
//! refused: throttled ones, and for updates (idempotent) a 5xx too. A create
//! that failed with a 5xx may have been applied, so it is reported, not
//! re-sent.
void FlushBatch(ListWriteGlobalState &g) {
	if (g.pending.empty()) {
		return;
	}
	GraphClient client(g.target.auth);
	const string items_path = g.target.base.substr(5) + "/items"; // $batch urls are relative to /v1.0
	vector<idx_t> todo;
	for (idx_t i = 0; i < g.pending.size(); i++) {
		todo.push_back(i);
	}
	for (int attempt = 1; !todo.empty(); attempt++) {
		picojson::array requests;
		bool any_create = false;
		for (auto i : todo) {
			auto &p = g.pending[i];
			picojson::object r;
			r["id"] = picojson::value(std::to_string(i));
			picojson::object headers;
			headers["Content-Type"] = picojson::value("application/json");
			r["headers"] = picojson::value(headers);
			if (p.item_id.empty()) {
				any_create = true;
				picojson::object body;
				body["fields"] = picojson::value(p.fields);
				r["method"] = picojson::value("POST");
				r["url"] = picojson::value(items_path);
				r["body"] = picojson::value(body);
			} else {
				r["method"] = picojson::value("PATCH");
				r["url"] = picojson::value(items_path + "/" + EncodeWorkbookName(p.item_id) + "/fields");
				r["body"] = picojson::value(p.fields);
			}
			requests.emplace_back(r);
		}
		picojson::object batch;
		batch["requests"] = picojson::value(requests);
		picojson::value holder;
		const auto &reply = AsObject(
		    client.GraphSend("POST", "/v1.0/$batch", picojson::value(batch).serialize(), g.target.what, !any_create),
		    g.target.what, holder);
		auto responses = reply.find("responses");
		if (responses == reply.end() || !responses->second.is<picojson::array>()) {
			throw IOException("sharepoint: unexpected $batch reply writing %s", g.target.what);
		}
		vector<idx_t> again;
		int retry_after = 0;
		for (auto &resp : responses->second.get<picojson::array>()) {
			if (!resp.is<picojson::object>()) {
				continue;
			}
			auto &o = resp.get<picojson::object>();
			const auto index = static_cast<idx_t>(std::stoull(StringField(o, "id")));
			auto status_it = o.find("status");
			const int status = (status_it != o.end() && status_it->second.is<int64_t>())
			                       ? static_cast<int>(status_it->second.get<int64_t>())
			                       : 0;
			if (status >= 200 && status < 300) {
				continue;
			}
			string body = "{}";
			auto body_it = o.find("body");
			if (body_it != o.end()) {
				body = body_it->second.serialize();
			}
			string code, message;
			picojson::value err;
			if (picojson::parse(err, body).empty() && err.is<picojson::object>()) {
				auto e = err.get<picojson::object>().find("error");
				if (e != err.get<picojson::object>().end() && e->second.is<picojson::object>()) {
					code = StringField(e->second.get<picojson::object>(), "code");
					message = StringField(e->second.get<picojson::object>(), "message");
				}
			}
			const bool create = g.pending[index].item_id.empty();
			const auto kind = ClassifyGraphError(status, code);
			const bool retry = create ? IsRetryableWhenNotIdempotent(kind) : IsRetryable(kind);
			if (!retry || attempt >= kMaxBatchAttempts) {
				const string row = create ? "a new item" : "item " + g.pending[index].item_id;
				throw IOException("sharepoint: writing %s to %s failed: HTTP %d %s: %s%s", row, g.target.what, status,
				                  code, message,
				                  create && !retry && status >= 500 ? " (it may or may not have been created)" : "");
			}
			auto headers = o.find("headers");
			if (headers != o.end() && headers->second.is<picojson::object>()) {
				retry_after = std::max(retry_after, ParseRetryAfterSeconds(StringField(
				                                        headers->second.get<picojson::object>(), "Retry-After")));
			}
			again.push_back(index);
		}
		if (!again.empty()) {
			IncrementCounter(Counter::RETRY);
			SleepBeforeRetry(attempt, retry_after);
		}
		todo = std::move(again);
	}
	g.pending.clear();
}

unique_ptr<LocalFunctionData> ListWriteInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

void ListWriteSink(ExecutionContext &, FunctionData &, GlobalFunctionData &gstate_p, LocalFunctionData &,
                   DataChunk &input) {
	auto &g = gstate_p.Cast<ListWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		for (idx_t r = 0; r < input.size(); r++) {
			PendingRequest p;
			for (idx_t c = 0; c < input.ColumnCount(); c++) {
				auto &col = g.columns[c];
				if (col.target == ListWriteTarget::SKIP) {
					continue;
				}
				auto v = input.GetValue(c, r);
				if (col.target == ListWriteTarget::ID) {
					if (!v.IsNull()) {
						p.item_id = v.ToString();
					}
					continue;
				}
				auto json = ToJsonField(v, col.field);
				if (json.is<picojson::array>()) {
					// A multi-choice value: Graph needs the collection's type.
					p.fields[col.field + "@odata.type"] = picojson::value("Collection(Edm.String)");
				}
				p.fields[col.field] = std::move(json);
			}
			g.pending.push_back(std::move(p));
			if (g.pending.size() >= kBatchRequests) {
				FlushBatch(g);
			}
		}
	});
}

void ListWriteCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

void ListWriteFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate_p) {
	auto &g = gstate_p.Cast<ListWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, { FlushBatch(g); });
}

CopyFunctionExecutionMode ListWriteExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

void ListCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, kInPlaceMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, "site", LogicalType::VARCHAR);
}

BoundStatement ListCopyPlan(Binder &binder, CopyStatement &stmt) {
	return RewriteCopyInPlace(binder, stmt, kListFormatInternal, "list");
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

	CopyFunction front(duckdb_compat::FunctionName(kListFormat));
	front.plan = ListCopyPlan;
	loader.RegisterFunction(front);

	CopyFunction writer(duckdb_compat::FunctionName(kListFormatInternal));
	writer.copy_options = ListCopyOptions;
	writer.copy_to_bind = ListWriteBind;
	writer.copy_to_initialize_global = ListWriteInitGlobal;
	writer.copy_to_initialize_local = ListWriteInitLocal;
	writer.copy_to_sink = ListWriteSink;
	writer.copy_to_combine = ListWriteCombine;
	writer.copy_to_finalize = ListWriteFinalize;
	writer.execution_mode = ListWriteExecutionMode;
	loader.RegisterFunction(writer);
}

} // namespace sharepoint
} // namespace duckdb
