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
#include "sharepoint_delta.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <picojson.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
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
//! People looked up per $batch call (Graph's $batch limit).
constexpr idx_t kPeoplePerBatch = 20;

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
	def.indexed = BoolField(o, "indexed");
	// The facet is the one object-valued key naming a kind.
	for (const char *facet : {"text", "number", "currency", "boolean", "dateTime", "choice", "lookup", "personOrGroup",
	                          "hyperlinkOrPicture", "calculated"}) {
		auto it = o.find(facet);
		if (it != o.end() && it->second.is<picojson::object>()) {
			def.facet = FacetOf(facet);
			if (def.facet == ListFacet::PERSON) {
				def.multi = BoolField(it->second.get<picojson::object>(), "allowMultipleSelection");
			}
			if (def.facet == ListFacet::CHOICE) {
				const auto shown = StringField(it->second.get<picojson::object>(), "displayAs");
				def.single_choice = shown == "dropDownMenu" || shown == "radioButtons";
			}
			// dateTime and calculated both say "format": "dateOnly" | "dateTime".
			def.date_only = StringField(it->second.get<picojson::object>(), "format") == "dateOnly";
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
	case ListColumnType::BIGINT_LIST:
		return LogicalType::LIST(LogicalType::BIGINT);
	case ListColumnType::DATE:
		return LogicalType::DATE;
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
	case ListColumnType::DATE: {
		int64_t us;
		if (v->is<string>() && ParseIsoTimestamp(v->get<string>(), us)) {
			return Value::DATE(date_t(DateOnlyDays(us)));
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

//! Every item of a list, as its `fields` object, page by page; `filter` is
//! an OData $filter ("" for none).
vector<picojson::object> FetchItems(GraphClient &client, const string &base, const string &select, int64_t page_size,
                                    const string &filter, const string &what) {
	vector<picojson::object> items;
	string next = base + "/items?$expand=fields($select=" + select + ")&$top=" + std::to_string(page_size);
	if (!filter.empty()) {
		next += "&$filter=" + EncodeWorkbookName(filter);
	}
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
				items.push_back(fields != item.get<picojson::object>().end() && fields->second.is<picojson::object>()
				                    ? fields->second.get<picojson::object>()
				                    : picojson::object());
			}
		}
		auto link = page.find("@odata.nextLink");
		next = (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>())
		                                                         : string();
	}
	return items;
}

//! A person column's value from an item's fields (#63). The id comes from
//! <internal>LookupId (one id, or a list of them), or from the objects of
//! <internal> ([{LookupId, LookupValue, Email}], as Graph may send several
//! people); a name or email only from those objects -- else NULL, for
//! ListScan to look up in the User Information List.
Value PersonValue(const picojson::object &fields, const ListOutputColumn &col) {
	const bool multi = col.type == ListColumnType::VARCHAR_LIST || col.type == ListColumnType::BIGINT_LIST;
	const auto type = ToLogicalType(col.type);
	const auto element = multi ? ListType::GetChildType(type) : type;
	vector<Value> values;
	auto objects = fields.find(col.person_field);
	if (objects != fields.end() && objects->second.is<picojson::array>()) {
		for (auto &o : objects->second.get<picojson::array>()) {
			if (!o.is<picojson::object>()) {
				continue;
			}
			auto &person = o.get<picojson::object>();
			const char *key = col.person == ListPersonPart::NAME ? "LookupValue"
			                  : col.person == ListPersonPart::EMAIL ? "Email"
			                                                        : "LookupId";
			auto it = person.find(key);
			values.push_back(ToValue(it == person.end() ? nullptr : &it->second,
			                         col.person == ListPersonPart::ID ? ListColumnType::BIGINT
			                                                          : ListColumnType::VARCHAR));
		}
	} else if (col.person == ListPersonPart::ID) {
		auto ids = fields.find(col.field);
		if (ids != fields.end() && ids->second.is<picojson::array>()) {
			for (auto &id : ids->second.get<picojson::array>()) {
				values.push_back(ToValue(&id, ListColumnType::BIGINT));
			}
		} else if (ids != fields.end()) {
			values.push_back(ToValue(&ids->second, ListColumnType::BIGINT));
		}
	} else {
		return Value(type); // looked up by ListScan
	}
	if (multi) {
		return values.empty() ? Value(type) : Value::LIST(element, std::move(values));
	}
	return values.empty() ? Value(type) : values[0].DefaultCastAs(type);
}

vector<vector<Value>> RowsOf(const vector<picojson::object> &items, const vector<ListOutputColumn> &plan) {
	vector<vector<Value>> rows;
	rows.reserve(items.size());
	for (auto &fields : items) {
		vector<Value> row;
		row.reserve(plan.size());
		for (auto &col : plan) {
			if (col.person != ListPersonPart::NONE) {
				row.push_back(PersonValue(fields, col));
				continue;
			}
			auto it = fields.find(col.field);
			row.push_back(ToValue(it == fields.end() ? nullptr : &it->second, col.type));
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

struct ListBindData : public TableFunctionData {
	vector<ListColumnType> types;
	//! Read at bind (a choice column may hold several values, which only the
	//! data shows, and the type must be known at bind) ...
	vector<vector<Value>> rows;
	//! ... or, when `deferred`, at init, with the WHERE predicates on indexed
	//! columns pushed down as `filter` (#61).
	bool deferred = false;
	AuthContext auth;
	string base;
	string select;
	string what;
	int64_t page_size = 0;
	vector<ListOutputColumn> plan;
	string filter;
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
	int status = 0;
	auto body = client.GraphGet("/v1.0/sites/" + address + "?$select=id", what, &status);
	if (status == 404) {
		// A 404 here is a mistyped site -- or, possibly, one an app with
		// Sites.Selected was not granted (unconfirmed live, hld §7a), so the
		// grant hint goes here as well as on the 403.
		throw InvalidInputException("sharepoint: %s not found -- check the name, or, for an app with Sites.Selected, "
		                            "grant it this site (README: Least privilege)",
		                            what);
	}
	const auto &found = AsObject(body, what, site_holder);
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

		// Name every planned field: a lookup's display value is only returned
		// when it is selected explicitly (by default Graph sends <name>LookupId
		// alone -- found live).
		string select;
		std::set<string> selected;
		for (auto &col : plan) {
			// Several people may come only as <internal>: [{LookupId,
			// LookupValue, Email}], so it is asked for too.
			const bool multi_person = col.person != ListPersonPart::NONE && col.type != ListColumnType::BIGINT &&
			                          col.type != ListColumnType::VARCHAR;
			for (auto &field : {col.field, multi_person ? col.person_field : string()}) {
				if (!field.empty() && selected.insert(field).second) {
					select += (select.empty() ? "" : ",") + EncodeWorkbookName(field);
				}
			}
		}
		const auto page_size = ListPageSize(context);
		auto data = make_uniq<ListBindData>();
		data->deferred = std::none_of(plan.begin(), plan.end(), [](const ListOutputColumn &c) { return c.may_be_multi; });
		if (!data->deferred) {
			// Every item first: whether a choice column is multi-valued shows
			// only in the data.
			auto items = FetchItems(client, base, select, page_size, "", what);
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
			data->rows = RowsOf(items, plan);
		}
		for (auto &col : plan) {
			data->types.push_back(col.type);
			return_types.push_back(ToLogicalType(col.type));
			names.emplace_back(col.name);
		}
		data->auth = target.auth;
		data->base = base;
		data->select = select;
		data->what = what;
		data->page_size = page_size;
		data->plan = plan;
		result = std::move(data);
	});
	return result;
}

// --- WHERE pushdown (#61) ---------------------------------------------------
// The filters stay where they are: DuckDB applies each one after the scan, so
// a pushed $filter only has to let through at least the rows the SQL keeps
// (ODataClause). Expressions are walked with ExpressionIterator and expression
// types only: 2.0 turned comparisons into function expressions.

namespace detail {
template <class E>
auto Binding(const E &expr, int) -> decltype(expr.Binding()) {
	return expr.Binding();
}
template <class E>
auto Binding(const E &expr, long) -> decltype((expr.binding)) {
	return expr.binding;
}
} // namespace detail

vector<reference<const Expression>> ChildrenOf(const Expression &expr) {
	vector<reference<const Expression>> out;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { out.push_back(child); });
	return out;
}

//! The plan column `expr` reads, when it is a plain column of this scan.
const ListOutputColumn *ColumnOf(const Expression &expr, const LogicalGet &get, const ListBindData &data) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	const auto &binding = detail::Binding(expr.Cast<BoundColumnRefExpression>(), 0);
	if (!(binding.table_index == get.table_index)) {
		return nullptr;
	}
	auto &ids = get.GetColumnIds();
	const idx_t pos = binding.column_index;
	if (pos >= ids.size() || ids[pos].IsRowIdColumn() || ids[pos].IsVirtualColumn()) {
		return nullptr;
	}
	const auto col = ids[pos].GetPrimaryIndex();
	return col < data.plan.size() ? &data.plan[col] : nullptr;
}

//! Add the constant to `p` as its kind's literal; false if it is not one.
bool AddLiteral(const Expression &expr, ListPredicate &p) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &v = duckdb_compat::ConstantValue(expr.Cast<BoundConstantExpression>());
	if (v.IsNull()) {
		return false;
	}
	switch (p.kind) {
	case ListFilterKind::TEXT:
		if (v.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		p.texts.push_back(StringValue::Get(v));
		return true;
	case ListFilterKind::NUMBER:
		if (!v.type().IsNumeric()) {
			return false;
		}
		p.numbers.push_back(v.GetValue<double>());
		// An integer past 2^53 is not a double: its = would be stricter.
		if (v.type().IsIntegral() && std::fabs(p.numbers.back()) > 9007199254740992.0) {
			return false;
		}
		return true;
	case ListFilterKind::TIMESTAMP:
		if (v.type().id() != LogicalTypeId::TIMESTAMP) {
			return false;
		}
		p.micros = Timestamp::GetEpochMicroSeconds(v.GetValue<timestamp_t>());
		return true;
	default:
		return false;
	}
}

bool ToPredicate(const Expression &expr, const LogicalGet &get, const ListBindData &data, ListPredicate &p) {
	auto children = ChildrenOf(expr);
	ListFilterOp op;
	bool flip = false;
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
		op = ListFilterOp::EQ;
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		op = ListFilterOp::LT;
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		op = ListFilterOp::LE;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		op = ListFilterOp::GT;
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		op = ListFilterOp::GE;
		break;
	case ExpressionType::COMPARE_IN:
		op = ListFilterOp::IN;
		break;
	case ExpressionType::BOUND_FUNCTION: {
		// col LIKE 'abc%' is rewritten to prefix(col, 'abc').
		const auto text = expr.ToString();
		if (!StringUtil::StartsWith(text, "prefix(") && !StringUtil::StartsWith(text, "starts_with(")) {
			return false;
		}
		op = ListFilterOp::PREFIX;
		break;
	}
	default:
		return false;
	}
	if (children.size() < 2) {
		return false;
	}
	auto col = ColumnOf(children[0], get, data);
	if (!col && op != ListFilterOp::IN && op != ListFilterOp::PREFIX && children.size() == 2) {
		col = ColumnOf(children[1], get, data); // 5 < Amount
		flip = true;
	}
	if (!col || col->filter == ListFilterKind::NONE) {
		return false;
	}
	if (op != ListFilterOp::IN && children.size() != 2) {
		return false;
	}
	p.field = col->field;
	p.kind = col->filter;
	p.op = op;
	if (flip) {
		p.op = op == ListFilterOp::LT   ? ListFilterOp::GT
		       : op == ListFilterOp::LE ? ListFilterOp::GE
		       : op == ListFilterOp::GT ? ListFilterOp::LT
		       : op == ListFilterOp::GE ? ListFilterOp::LE
		                                : op;
		return AddLiteral(children[0], p);
	}
	for (idx_t i = 1; i < children.size(); i++) {
		if (!AddLiteral(children[i], p)) {
			return false;
		}
	}
	return true;
}

void ListPushdownFilter(ClientContext &, LogicalGet &get, FunctionData *bind_data,
                        vector<unique_ptr<Expression>> &filters) {
	auto &data = bind_data->Cast<ListBindData>();
	if (!data.deferred) {
		return;
	}
	vector<ListPredicate> predicates;
	for (auto &filter : filters) {
		ListPredicate p;
		if (ToPredicate(*filter, get, data, p)) {
			predicates.push_back(std::move(p));
		} else if (filter->GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			// DuckDB folds `a > x AND a < y` into a BETWEEN (found live): two
			// inclusive bounds (ODataClause widens either way).
			auto children = ChildrenOf(*filter);
			auto col = children.size() == 3 ? ColumnOf(children[0], get, data) : nullptr;
			if (!col || col->filter == ListFilterKind::NONE) {
				continue;
			}
			ListPredicate lower, upper;
			for (auto bound : {&lower, &upper}) {
				bound->field = col->field;
				bound->kind = col->filter;
			}
			lower.op = ListFilterOp::GE;
			upper.op = ListFilterOp::LE;
			if (AddLiteral(children[1], lower) && AddLiteral(children[2], upper)) {
				predicates.push_back(std::move(lower));
				predicates.push_back(std::move(upper));
			}
		}
	}
	data.filter = BuildListFilter(predicates);
}

//! EXPLAIN shows the list and the $filter sent to Graph.
InsertionOrderPreservingMap<string> ListToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &data = input.bind_data->Cast<ListBindData>();
	result["List"] = data.what;
	if (!data.filter.empty()) {
		result["Graph $filter"] = data.filter;
	}
	return result;
}

//! The id of the site's User Information List, or "" if it cannot be read.
//! Not by its title: it is hidden, Graph answers lists/User Information List
//! with 404 (live CI, 2026-11-03), and the title is in the site's language.
//! It is the system list at _catalogs/users; system lists are listed only
//! when `system` is selected.
string UserListId(GraphClient &client, const string &site, const string &what) {
	try {
		string next = "/v1.0" + site + "/lists?$select=id,webUrl,system&$top=999";
		for (int pages = 0; !next.empty() && pages < 50; pages++) {
			picojson::value holder;
			const auto &page = AsObject(client.GraphGet(next, what), what, holder);
			auto values = page.find("value");
			if (values != page.end() && values->second.is<picojson::array>()) {
				for (auto &v : values->second.get<picojson::array>()) {
					if (v.is<picojson::object>() &&
					    StringUtil::EndsWith(StringUtil::Lower(StringField(v.get<picojson::object>(), "webUrl")),
					                         "/_catalogs/users")) {
						return StringField(v.get<picojson::object>(), "id");
					}
				}
			}
			auto link = page.find("@odata.nextLink");
			next = (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>())
			                                                         : string();
		}
	} catch (std::exception &) {
	}
	return string();
}

//! A person's name and email in the site's User Information List.
struct PersonInfo {
	Value name;
	Value email;
};

//! Look the people up in the User Information List of `site` ("/sites/{id}",
//! as $batch urls are relative to /v1.0), in $batch calls of 20. Best effort:
//! a person not found (removed from the site's list) or a refused call leaves
//! their name and email NULL.
unordered_map<int64_t, PersonInfo> ResolvePeople(GraphClient &client, const string &site, const string &what,
                                                 const vector<int64_t> &ids) {
	unordered_map<int64_t, PersonInfo> people;
	const auto users = UserListId(client, site, what);
	if (users.empty()) {
		return people;
	}
	for (idx_t start = 0; start < ids.size(); start += kPeoplePerBatch) {
		picojson::array requests;
		for (idx_t i = start; i < ids.size() && i < start + kPeoplePerBatch; i++) {
			picojson::object r;
			r["id"] = picojson::value(std::to_string(ids[i]));
			r["method"] = picojson::value("GET");
			r["url"] = picojson::value(site + "/lists/" + users + "/items/" + std::to_string(ids[i]) +
			                           "?$select=id&$expand=fields($select=Title,EMail)");
			requests.emplace_back(r);
		}
		picojson::object batch;
		batch["requests"] = picojson::value(requests);
		picojson::value holder;
		try {
			const auto &reply = AsObject(
			    client.GraphSend("POST", "/v1.0/$batch", picojson::value(batch).serialize(), what, true), what,
			    holder);
			auto responses = reply.find("responses");
			if (responses == reply.end() || !responses->second.is<picojson::array>()) {
				continue;
			}
			for (auto &resp : responses->second.get<picojson::array>()) {
				if (!resp.is<picojson::object>()) {
					continue;
				}
				auto &o = resp.get<picojson::object>();
				auto body = o.find("body");
				if (body == o.end() || !body->second.is<picojson::object>()) {
					continue;
				}
				auto fields = body->second.get<picojson::object>().find("fields");
				if (fields == body->second.get<picojson::object>().end() || !fields->second.is<picojson::object>()) {
					continue;
				}
				auto &f = fields->second.get<picojson::object>();
				auto title = StringField(f, "Title");
				auto email = StringField(f, "EMail");
				people[std::stoll(StringField(o, "id"))] = {title.empty() ? Value() : Value(title),
				                                             email.empty() ? Value() : Value(email)};
			}
		} catch (std::exception &) {
			// Names are a convenience; the ids are in the result either way.
		}
	}
	return people;
}

struct CursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
	//! The rows of a deferred read (ListBindData::deferred).
	vector<vector<Value>> rows;
	//! Projection pushdown: the plan column of each output column.
	vector<column_t> column_ids;
	//! For a person's name or email column: the plan index of its id column.
	unordered_map<idx_t, idx_t> id_column;
	unordered_map<int64_t, PersonInfo> people;
};

unique_ptr<GlobalTableFunctionState> CursorInit(ClientContext &, TableFunctionInitInput &input) {
	auto state = make_uniq<CursorState>();
	auto &data = input.bind_data->Cast<ListBindData>();
	state->column_ids = input.column_ids;
	GraphClient client(data.auth);
	if (data.deferred) {
		vector<picojson::object> items;
		if (data.filter.empty()) {
			items = FetchItems(client, data.base, data.select, data.page_size, "", data.what);
		} else {
			try {
				items = FetchItems(client, data.base, data.select, data.page_size, data.filter, data.what);
			} catch (std::exception &) {
				// A filter Graph refuses costs speed, never the result: read
				// everything and let DuckDB filter.
				items = FetchItems(client, data.base, data.select, data.page_size, "", data.what);
			}
		}
		state->rows = RowsOf(items, data.plan);
	}
	// People are looked up only when a name or email column is read.
	for (auto c : state->column_ids) {
		if (c >= data.plan.size() || (data.plan[c].person != ListPersonPart::NAME &&
		                              data.plan[c].person != ListPersonPart::EMAIL)) {
			continue;
		}
		for (idx_t i = 0; i < data.plan.size(); i++) {
			if (data.plan[i].person == ListPersonPart::ID && data.plan[i].field == data.plan[c].field) {
				state->id_column[c] = i;
			}
		}
	}
	if (!state->id_column.empty()) {
		std::set<int64_t> ids;
		auto &rows = data.deferred ? state->rows : data.rows;
		for (auto &row : rows) {
			for (auto &named : state->id_column) {
				if (!row[named.first].IsNull()) {
					continue; // Graph sent the name with the item
				}
				auto &id = row[named.second];
				if (id.IsNull()) {
					continue;
				}
				if (id.type().id() == LogicalTypeId::LIST) {
					for (auto &e : ListValue::GetChildren(id)) {
						if (!e.IsNull()) {
							ids.insert(e.GetValue<int64_t>());
						}
					}
				} else {
					ids.insert(id.GetValue<int64_t>());
				}
			}
		}
		// "/v1.0/sites/{site}/lists/{id}" -> "/sites/{site}".
		const auto site = data.base.substr(5, data.base.find("/lists/") - 5);
		state->people = ResolvePeople(client, site, data.what, vector<int64_t>(ids.begin(), ids.end()));
	}
	return std::move(state);
}

//! A person's name or email, looked up by the id(s) in the row.
Value LookedUp(const CursorState &state, const ListOutputColumn &col, const Value &id) {
	const auto type = ToLogicalType(col.type);
	auto one = [&](const Value &v) {
		if (v.IsNull()) {
			return Value(LogicalType::VARCHAR);
		}
		auto it = state.people.find(v.GetValue<int64_t>());
		if (it == state.people.end()) {
			return Value(LogicalType::VARCHAR);
		}
		const auto &found = col.person == ListPersonPart::NAME ? it->second.name : it->second.email;
		return found.IsNull() ? Value(LogicalType::VARCHAR) : found;
	};
	if (id.IsNull()) {
		return Value(type);
	}
	if (id.type().id() != LogicalTypeId::LIST) {
		return one(id);
	}
	vector<Value> out;
	for (auto &e : ListValue::GetChildren(id)) {
		out.push_back(one(e));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(out));
}

void ListScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ListBindData>();
	auto &state = input.global_state->Cast<CursorState>();
	auto &rows = data.deferred ? state.rows : data.rows;
	idx_t out = 0;
	while (state.cursor < rows.size() && out < STANDARD_VECTOR_SIZE) {
		auto &row = rows[state.cursor];
		for (idx_t c = 0; c < state.column_ids.size(); c++) {
			const auto col = state.column_ids[c];
			if (col >= row.size()) {
				output.SetValue(c, out, Value(output.data[c].GetType())); // the row id: none
				continue;
			}
			auto named = state.id_column.find(col);
			if (named != state.id_column.end() && row[col].IsNull()) {
				output.SetValue(c, out, LookedUp(state, data.plan[col], row[named->second]));
			} else {
				output.SetValue(c, out, row[col]);
			}
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
				auto json = col.date_only && !v.IsNull()
				                ? picojson::value(FormatDateOnly(v.DefaultCastAs(LogicalType::DATE).GetValue<date_t>().days))
				                : ToJsonField(v, col.field);
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


// ===========================================================================
// read_sharepoint_files('sharepoint://Finance/**/*.xlsx') (#65): a document
// library's files and their metadata, one row per file. A library is a list,
// so its own columns are planned as read_sharepoint_list plans a list's.
// ===========================================================================

//! Files whose metadata is fetched per $batch call (Graph's limit).
constexpr idx_t kFilesPerBatch = 20;

//! The fixed columns, before the library's own.
const vector<std::pair<string, LogicalType>> &FileColumns() {
	static const vector<std::pair<string, LogicalType>> columns = {
	    {"path", LogicalType::VARCHAR},          {"name", LogicalType::VARCHAR},
	    {"size", LogicalType::BIGINT},           {"created", LogicalType::TIMESTAMP},
	    {"modified", LogicalType::TIMESTAMP},    {"created_by", LogicalType::VARCHAR},
	    {"created_by_email", LogicalType::VARCHAR}, {"modified_by", LogicalType::VARCHAR},
	    {"modified_by_email", LogicalType::VARCHAR}, {"content_type", LogicalType::VARCHAR},
	    {"id", LogicalType::VARCHAR},            {"web_url", LogicalType::VARCHAR}};
	return columns;
}

//! A root-relative path for /root:/<here>, each segment percent-encoded.
string EncodeItemPath(const string &path) {
	string out;
	for (auto &segment : StringUtil::Split(path, '/')) {
		out += (out.empty() ? "" : "/") + EncodeWorkbookName(segment);
	}
	return out;
}

Value TimestampField(const picojson::object &o, const char *key) {
	int64_t us;
	const auto text = StringField(o, key);
	return !text.empty() && ParseIsoTimestamp(text, us) ? Value::TIMESTAMP(timestamp_t(us))
	                                                      : Value(LogicalType::TIMESTAMP);
}

//! displayName / email of a driveItem's createdBy / lastModifiedBy.
std::pair<Value, Value> Who(const picojson::object &item, const char *key) {
	auto by = item.find(key);
	if (by == item.end() || !by->second.is<picojson::object>()) {
		return {Value(LogicalType::VARCHAR), Value(LogicalType::VARCHAR)};
	}
	const auto &o = by->second.get<picojson::object>();
	for (const char *kind : {"user", "application"}) {
		auto who = o.find(kind);
		if (who != o.end() && who->second.is<picojson::object>()) {
			const auto name = StringField(who->second.get<picojson::object>(), "displayName");
			const auto email = StringField(who->second.get<picojson::object>(), "email");
			return {name.empty() ? Value(LogicalType::VARCHAR) : Value(name),
			        email.empty() ? Value(LogicalType::VARCHAR) : Value(email)};
		}
	}
	return {Value(LogicalType::VARCHAR), Value(LogicalType::VARCHAR)};
}

//! A failed part of a $batch, as the exception its kind maps to (403 ->
//! PermissionException, with the Sites.Selected hint).
[[noreturn]] void ThrowBatchPart(const string &what, int status, const string &body) {
	string code, message;
	picojson::value err;
	if (picojson::parse(err, body).empty() && err.is<picojson::object>()) {
		auto e = err.get<picojson::object>().find("error");
		if (e != err.get<picojson::object>().end() && e->second.is<picojson::object>()) {
			code = StringField(e->second.get<picojson::object>(), "code");
			message = StringField(e->second.get<picojson::object>(), "message");
		}
	}
	const auto text = FormatGraphError("reading " + what, status, code, message);
	switch (ExceptionTypeFor(ClassifyGraphError(status, code))) {
	case GraphExceptionType::PERMISSION:
		throw PermissionException(text);
	case GraphExceptionType::INVALID_INPUT:
		throw InvalidInputException(text);
	default:
		throw IOException(text);
	}
}

struct FilesBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

unique_ptr<FunctionData> FilesBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the path must not be NULL");
		}
		const auto arg = StringValue::Get(input.inputs[0]);
		const string pattern = IsSharepointUri(arg) ? arg : string(SHAREPOINT_SCHEME) + arg;
		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, pattern);
		GraphClient client(auth);
		const string what = "the files of '" + pattern + "'";

		// The library's own columns, planned as a list's; the system ones
		// (id, created/modified and their people) are the fixed columns here.
		vector<ListColumnDef> defs;
		{
			picojson::value holder;
			const auto &cols = AsObject(client.GraphGet("/v1.0/drives/" + auth.drive_id + "/list/columns", what),
			                            what, holder);
			auto values = cols.find("value");
			if (values != cols.end() && values->second.is<picojson::array>()) {
				for (auto &c : values->second.get<picojson::array>()) {
					if (c.is<picojson::object>()) {
						auto def = ParseColumnDef(c.get<picojson::object>());
						// The file name is `name`; Title stays (a document's title).
						if (def.name != "FileLeafRef") {
							defs.push_back(def);
						}
					}
				}
			}
		}
		vector<ListOutputColumn> plan;
		for (auto &col : PlanListColumns(defs)) {
			const bool system = col.field == "id" || col.field == "Created" || col.field == "Modified" ||
			                    col.person_field == "Author" || col.person_field == "Editor";
			if (!system) {
				// A multi-valued choice reads as a list; its type cannot wait for
				// the data here (one row per file), so a choice is VARCHAR[] only
				// when its definition says several values.
				col.may_be_multi = false;
				plan.push_back(col);
			}
		}

		// The files: the filesystem's glob (its syntax, its immutable-prefix
		// cache), then each file's driveItem with its listItem fields.
		auto &fs = FileSystem::GetFileSystem(context);
		const auto matches = fs.Glob(pattern); // the opener is pushed by DuckDB (passing one throws)
		string select;
		std::set<string> selected;
		for (auto &col : plan) {
			for (auto &field : {col.field, col.person != ListPersonPart::NONE && col.type != ListColumnType::BIGINT &&
			                                        col.type != ListColumnType::VARCHAR
			                                    ? col.person_field
			                                    : string()}) {
				if (!field.empty() && selected.insert(field).second) {
					select += (select.empty() ? "" : ",") + EncodeWorkbookName(field);
				}
			}
		}
		const string query = "?$select=id,name,size,createdDateTime,lastModifiedDateTime,createdBy,lastModifiedBy,"
		                     "file,folder,webUrl&$expand=listItem($select=id;$expand=fields" +
		                     (select.empty() ? string() : "($select=" + select + ")") + ")";
		vector<std::pair<string, picojson::object>> items; // path, driveItem
		for (idx_t start = 0; start < matches.size(); start += kFilesPerBatch) {
			std::map<string, string> pending; // request id -> path
			for (idx_t i = start; i < matches.size() && i < start + kFilesPerBatch; i++) {
				pending[std::to_string(i)] = matches[i].path;
			}
			std::map<string, picojson::object> found;
			for (int attempt = 1; !pending.empty(); attempt++) {
				picojson::array requests;
				for (auto &p : pending) {
					auto parsed = ParseSharepointUri(p.second);
					if (!parsed.ok || parsed.uri.kind != SharepointUriKind::PATH) {
						continue;
					}
					picojson::object r;
					r["id"] = picojson::value(p.first);
					r["method"] = picojson::value("GET");
					r["url"] = picojson::value("/drives/" + auth.drive_id + "/root:/" +
					                           EncodeItemPath(parsed.uri.RootRelativePath()) + query);
					requests.emplace_back(r);
				}
				picojson::object batch;
				batch["requests"] = picojson::value(requests);
				picojson::value holder;
				const auto &reply = AsObject(
				    client.GraphSend("POST", "/v1.0/$batch", picojson::value(batch).serialize(), what, true), what,
				    holder);
				auto responses = reply.find("responses");
				if (responses == reply.end() || !responses->second.is<picojson::array>()) {
					throw IOException("sharepoint: unexpected $batch reply reading %s", what);
				}
				int retry_after = 0;
				for (auto &resp : responses->second.get<picojson::array>()) {
					if (!resp.is<picojson::object>()) {
						continue;
					}
					auto &o = resp.get<picojson::object>();
					const auto id = StringField(o, "id");
					auto status = o.find("status");
					const int code = status != o.end() && status->second.is<int64_t>()
					                     ? static_cast<int>(status->second.get<int64_t>())
					                     : 0;
					auto body = o.find("body");
					if (code >= 200 && code < 300 && body != o.end() && body->second.is<picojson::object>()) {
						found[id] = body->second.get<picojson::object>();
						pending.erase(id);
					} else if (code == 404) {
						pending.erase(id); // gone since the glob listed it
					} else if (code == 429 || code == 503 || code >= 500) {
						retry_after = std::max(retry_after, 1);
					} else {
						const auto text = body != o.end() ? body->second.serialize() : string();
						ThrowBatchPart(what, code, text);
					}
				}
				if (!pending.empty()) {
					if (attempt >= kMaxBatchAttempts) {
						throw IOException("sharepoint: reading %s: Graph kept throttling the metadata requests", what);
					}
					SleepBeforeRetry(attempt, retry_after);
				}
			}
			for (auto &f : found) {
				items.emplace_back(matches[std::stoull(f.first)].path, std::move(f.second));
			}
		}

		// People in the library's own person columns: names and emails from
		// the site's User Information List.
		auto data = make_uniq<FilesBindData>();
		vector<vector<Value>> custom;
		std::set<int64_t> person_ids;
		for (auto &item : items) {
			picojson::object fields;
			auto list_item = item.second.find("listItem");
			if (list_item != item.second.end() && list_item->second.is<picojson::object>()) {
				auto f = list_item->second.get<picojson::object>().find("fields");
				if (f != list_item->second.get<picojson::object>().end() && f->second.is<picojson::object>()) {
					fields = f->second.get<picojson::object>();
				}
			}
			custom.push_back(RowsOf({fields}, plan)[0]);
			for (idx_t c = 0; c < plan.size(); c++) {
				if (plan[c].person != ListPersonPart::ID) {
					continue;
				}
				auto &id = custom.back()[c];
				if (id.IsNull()) {
					continue;
				}
				if (id.type().id() == LogicalTypeId::LIST) {
					for (auto &e : ListValue::GetChildren(id)) {
						if (!e.IsNull()) {
							person_ids.insert(e.GetValue<int64_t>());
						}
					}
				} else {
					person_ids.insert(id.GetValue<int64_t>());
				}
			}
		}
		CursorState lookup; // LookedUp reads the people from it
		auto &people = lookup.people;
		if (!person_ids.empty()) {
			picojson::value holder;
			const auto &library = AsObject(
			    client.GraphGet("/v1.0/drives/" + auth.drive_id + "/list?$select=parentReference", what), what, holder);
			auto parent = library.find("parentReference");
			if (parent != library.end() && parent->second.is<picojson::object>()) {
				const auto site = StringField(parent->second.get<picojson::object>(), "siteId");
				people = ResolvePeople(client, "/sites/" + site, what,
				                       vector<int64_t>(person_ids.begin(), person_ids.end()));
			}
		}
		for (idx_t r = 0; r < items.size(); r++) {
			auto &item = items[r].second;
			if (item.find("folder") != item.end()) {
				continue; // a literal folder path globs to itself
			}
			string mime;
			auto file = item.find("file");
			if (file != item.end() && file->second.is<picojson::object>()) {
				mime = StringField(file->second.get<picojson::object>(), "mimeType");
			}
			auto size = item.find("size");
			const auto created_by = Who(item, "createdBy");
			const auto modified_by = Who(item, "lastModifiedBy");
			vector<Value> row = {Value(items[r].first),
			                     Value(StringField(item, "name")),
			                     size != item.end() && size->second.is<int64_t>()
			                         ? Value::BIGINT(size->second.get<int64_t>())
			                         : Value(LogicalType::BIGINT),
			                     TimestampField(item, "createdDateTime"),
			                     TimestampField(item, "lastModifiedDateTime"),
			                     created_by.first,
			                     created_by.second,
			                     modified_by.first,
			                     modified_by.second,
			                     mime.empty() ? Value(LogicalType::VARCHAR) : Value(mime),
			                     Value(StringField(item, "id")),
			                     Value(StringField(item, "webUrl"))};
			for (idx_t c = 0; c < plan.size(); c++) {
				auto value = custom[r][c];
				if (value.IsNull() && (plan[c].person == ListPersonPart::NAME || plan[c].person == ListPersonPart::EMAIL)) {
					for (idx_t i = 0; i < plan.size(); i++) {
						if (plan[i].person == ListPersonPart::ID && plan[i].field == plan[c].field) {
							value = LookedUp(lookup, plan[c], custom[r][i]);
						}
					}
				}
				row.push_back(std::move(value));
			}
			data->rows.push_back(std::move(row));
		}

		std::set<string> used;
		for (auto &col : FileColumns()) {
			names.emplace_back(col.first);
			return_types.push_back(col.second);
			used.insert(StringUtil::Lower(col.first));
		}
		for (auto &col : plan) {
			// A library column named like a fixed one ("Name", "Size") gets "_2".
			string name = col.name;
			for (int n = 2; used.count(StringUtil::Lower(name)); n++) {
				name = col.name + "_" + std::to_string(n);
			}
			used.insert(StringUtil::Lower(name));
			names.emplace_back(name);
			return_types.push_back(ToLogicalType(col.type));
		}
		result = std::move(data);
	});
	return result;
}

struct FilesCursor : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> FilesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<FilesCursor>();
}

void FilesScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<FilesBindData>();
	auto &state = input.global_state->Cast<FilesCursor>();
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
// sharepoint_list_changes('Orders', token := ...) (#62, lists): the items
// added, changed and deleted since a token -- Graph's list item delta. The
// list's own columns, then `_change` ('upsert' / 'delete' / 'checkpoint') and
// `token` (on the checkpoint row: the token for next time).
// ===========================================================================

struct ListChangesBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

unique_ptr<FunctionData> ListChangesBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the list name must not be NULL");
		}
		string site, token;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "site")) {
			if (!v->IsNull()) {
				site = StringValue::Get(*v);
			}
		}
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "token")) {
			if (!v->IsNull()) {
				token = StringValue::Get(*v);
			}
		}
		for (char c : token) {
			if (!(isalnum(static_cast<unsigned char>(c)) || c == '%' || c == '-' || c == '_' || c == '.' || c == '~' ||
			      c == '=')) {
				throw InvalidInputException("sharepoint: that is not a token sharepoint_list_changes returned");
			}
		}
		auto target = ResolveList(context, StringValue::Get(input.inputs[0]), site);
		GraphClient client(target.auth);
		auto &plan = target.plan;
		const string what = "the changes of " + target.what;

		string select;
		std::set<string> selected;
		for (auto &col : plan) {
			const bool multi_person = col.person != ListPersonPart::NONE && col.type != ListColumnType::BIGINT &&
			                          col.type != ListColumnType::VARCHAR;
			for (auto &field : {col.field, multi_person ? col.person_field : string()}) {
				if (!field.empty() && selected.insert(field).second) {
					select += (select.empty() ? "" : ",") + EncodeWorkbookName(field);
				}
			}
		}
		string next = target.base + "/items/delta?$expand=fields($select=" + select + ")" +
		              (token.empty() ? string() : "&token=" + token);
		vector<picojson::object> items;
		vector<bool> deleted;
		std::unordered_map<string, idx_t> index_of; // an item seen twice: its last state wins
		string next_token;
		for (int pages = 0; !next.empty(); pages++) {
			if (pages >= kMaxItemPages) {
				throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxItemPages);
			}
			picojson::value holder;
			try {
				AsObject(client.GraphGet(next, what), what, holder);
			} catch (std::exception &e) {
				const string text = e.what();
				if (text.find("HTTP 410") != string::npos || text.find("resyncRequired") != string::npos) {
					throw InvalidInputException("sharepoint: the token for %s has expired; start over with token := "
					                            "NULL (all items) or 'latest' (from now on)",
					                            what);
				}
				throw;
			}
			const auto &page = holder.get<picojson::object>();
			auto values = page.find("value");
			if (values != page.end() && values->second.is<picojson::array>()) {
				for (auto &v : values->second.get<picojson::array>()) {
					if (!v.is<picojson::object>()) {
						continue;
					}
					const auto &item = v.get<picojson::object>();
					const auto id = StringField(item, "id");
					const bool gone = item.find("deleted") != item.end() || item.find("@removed") != item.end();
					picojson::object fields;
					auto f = item.find("fields");
					if (!gone && f != item.end() && f->second.is<picojson::object>()) {
						fields = f->second.get<picojson::object>();
					}
					fields["id"] = picojson::value(id);
					auto seen = index_of.find(id);
					if (seen != index_of.end()) {
						items[seen->second] = std::move(fields);
						deleted[seen->second] = gone;
					} else {
						index_of[id] = items.size();
						items.push_back(std::move(fields));
						deleted.push_back(gone);
					}
				}
			}
			auto next_link = page.find("@odata.nextLink");
			auto delta_link = page.find("@odata.deltaLink");
			if (next_link != page.end() && next_link->second.is<string>()) {
				next = GraphPathOfLink(next_link->second.get<string>());
			} else {
				if (delta_link != page.end() && delta_link->second.is<string>()) {
					next_token = DeltaToken(delta_link->second.get<string>());
				}
				next.clear();
			}
		}
		if (next_token.empty()) {
			throw IOException("sharepoint: %s: Graph returned no delta token", what);
		}
		// A choice column is a list when any changed item says so (as
		// read_sharepoint_list decides from all items).
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
		auto rows = RowsOf(items, plan);
		// People's names and emails, as read_sharepoint_list looks them up.
		CursorState lookup;
		std::set<int64_t> person_ids;
		for (idx_t c = 0; c < plan.size(); c++) {
			if (plan[c].person != ListPersonPart::ID) {
				continue;
			}
			for (auto &row : rows) {
				auto &id = row[c];
				if (id.IsNull()) {
					continue;
				}
				if (id.type().id() == LogicalTypeId::LIST) {
					for (auto &e : ListValue::GetChildren(id)) {
						if (!e.IsNull()) {
							person_ids.insert(e.GetValue<int64_t>());
						}
					}
				} else {
					person_ids.insert(id.GetValue<int64_t>());
				}
			}
		}
		if (!person_ids.empty()) {
			lookup.people = ResolvePeople(client, "/sites/" + target.site_id, what,
			                              vector<int64_t>(person_ids.begin(), person_ids.end()));
		}
		auto data = make_uniq<ListChangesBindData>();
		for (idx_t r = 0; r < rows.size(); r++) {
			auto &row = rows[r];
			for (idx_t c = 0; c < plan.size(); c++) {
				if (!row[c].IsNull() ||
				    (plan[c].person != ListPersonPart::NAME && plan[c].person != ListPersonPart::EMAIL)) {
					continue;
				}
				for (idx_t i = 0; i < plan.size(); i++) {
					if (plan[i].person == ListPersonPart::ID && plan[i].field == plan[c].field) {
						row[c] = LookedUp(lookup, plan[c], row[i]);
					}
				}
			}
			row.push_back(Value(deleted[r] ? "delete" : "upsert"));
			row.push_back(Value(LogicalType::VARCHAR));
			data->rows.push_back(std::move(row));
		}
		vector<Value> checkpoint;
		for (auto &col : plan) {
			checkpoint.push_back(Value(ToLogicalType(col.type)));
		}
		checkpoint.push_back(Value("checkpoint"));
		checkpoint.push_back(Value(next_token));
		data->rows.push_back(std::move(checkpoint));

		for (auto &col : plan) {
			return_types.push_back(ToLogicalType(col.type));
			names.emplace_back(col.name);
		}
		names.emplace_back("_change");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("token");
		return_types.push_back(LogicalType::VARCHAR);
		result = std::move(data);
	});
	return result;
}

struct ListChangesCursor : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> ListChangesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ListChangesCursor>();
}

void ListChangesScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ListChangesBindData>();
	auto &state = input.global_state->Cast<ListChangesCursor>();
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
	fn.pushdown_complex_filter = ListPushdownFilter;
	fn.projection_pushdown = true;
	fn.to_string = ListToString;
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

	{
		TableFunction files("read_sharepoint_files", {LogicalType::VARCHAR}, FilesScan, FilesBind, FilesInit);
		duckdb_compat::NameParameters(files, {"path"});
		CreateTableFunctionInfo files_info(files);
		FunctionDescription files_desc;
		files_desc.description =
		    "A document library's files and their metadata, one row per file matching a sharepoint:// path or "
		    "glob: path, name, size, created/modified and by whom, content type, id, web URL, then the library's "
		    "own columns, typed as read_sharepoint_list types a list's.";
		files_desc.examples = {"SELECT name, size, modified_by FROM read_sharepoint_files('sharepoint://Finance/**/*.xlsx')"};
		files_desc.categories = {"sharepoint", "list"};
		files_info.descriptions.push_back(std::move(files_desc));
		loader.RegisterFunction(std::move(files_info));
	}

	{
		TableFunction changes("sharepoint_list_changes", {LogicalType::VARCHAR}, ListChangesScan, ListChangesBind,
		                      ListChangesInit);
		duckdb_compat::NameParameters(changes, {"list"});
		duckdb_compat::AddNamedParameter(changes, "token", LogicalType::VARCHAR);
		duckdb_compat::AddNamedParameter(changes, "site", LogicalType::VARCHAR);
		CreateTableFunctionInfo changes_info(changes);
		FunctionDescription changes_desc;
		changes_desc.description =
		    "The items of a SharePoint list added, changed or deleted since a token (Graph's delta query): the list's "
		    "columns as read_sharepoint_list gives them, then _change ('upsert', 'delete' -- only id is set -- or "
		    "'checkpoint') and token, set on the one checkpoint row: the token for next time. No token: every item; "
		    "token := 'latest': none, just a token.";
		changes_desc.examples = {"SELECT * FROM sharepoint_list_changes('Orders', token := 'aTE09...')"};
		changes_desc.categories = {"sharepoint", "list"};
		changes_info.descriptions.push_back(std::move(changes_desc));
		loader.RegisterFunction(std::move(changes_info));
	}

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
