// Planner and To Do tasks as tables (#68), over the same secret and Graph
// client as the rest of the extension:
//   read_planner_tasks(plan, group := ...)   -- /planner/plans/{id}/tasks
//   read_todo_tasks(list := ..., user := ...) -- /me (or /users/{u})/todo/lists/{id}/tasks
// Planner needs Tasks.Read.All (application) or Tasks.Read (delegated); To Do
// app-only needs a user := (there is no /me).
#include "sharepoint_tasks.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
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

constexpr int kMaxPages = 10000;

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

Value Time(const string &iso) {
	int64_t us;
	// To Do sends "2026-11-03T00:00:00.0000000" with a separate timeZone (UTC
	// for what Graph returns by default): read it as UTC.
	string s = iso;
	if (!s.empty() && s.back() != 'Z' && s.find('+') == string::npos && s.size() >= 19) {
		s += "Z";
	}
	return !s.empty() && ParseIsoTimestamp(s, us) ? Value::TIMESTAMP(timestamp_t(us)) : Value(LogicalType::TIMESTAMP);
}

string Param(TableFunctionBindInput &input, const char *name) {
	if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, name)) {
		if (!v->IsNull()) {
			return StringValue::Get(*v);
		}
	}
	return string();
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

//! Graph ids are spliced into URLs: only their alphabet passes.
string CheckedId(const string &id, const char *what) {
	for (char c : id) {
		if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@' || c == '!')) {
			throw InvalidInputException("sharepoint: '%s' is not a %s id or name", id, what);
		}
	}
	return id;
}

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

GraphClient Client(ClientContext &context, AuthContext &auth) {
	auto opener = ClientData::Get(context).file_opener.get();
	auth = GetAuthContext(opener, SHAREPOINT_SCHEME);
	return GraphClient(auth);
}

// --- Planner ---------------------------------------------------------------------
unique_ptr<FunctionData> PlannerBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
			throw InvalidInputException("sharepoint: read_planner_tasks needs a plan (its id, or its name with group := "
			                            "...)");
		}
		const auto plan = StringValue::Get(input.inputs[0]);
		const auto group = Param(input, "group");
		AuthContext auth;
		auto client = Client(context, auth);
		const string what = "Planner plan '" + plan + "'";
		// The plan: by id; or by name among the group's plans (app-only) or the
		// signed-in user's (delegated).
		string plan_id, plan_title;
		if (!group.empty() || plan.find(' ') != string::npos) {
			const string source = group.empty() ? "/v1.0/me/planner/plans"
			                                    : "/v1.0/groups/" + CheckedId(group, "group") + "/planner/plans";
			for (auto &p : All(client, source + "?$select=id,title", what)) {
				if (Str(p, "id") == plan || StringUtil::CIEquals(Str(p, "title"), plan)) {
					plan_id = Str(p, "id");
					plan_title = Str(p, "title");
					break;
				}
			}
			if (plan_id.empty()) {
				throw InvalidInputException("sharepoint: no %s%s", what,
				                            group.empty() ? " among your plans (app-only: pass group := ...)" : " in that group");
			}
		} else {
			plan_id = CheckedId(plan, "plan");
			picojson::value holder;
			if (picojson::parse(holder, client.GraphGet("/v1.0/planner/plans/" + plan_id + "?$select=id,title", what))
			        .empty() &&
			    holder.is<picojson::object>()) {
				plan_title = Str(holder.get<picojson::object>(), "title");
			}
		}
		std::unordered_map<string, string> buckets;
		for (auto &b : All(client, "/v1.0/planner/plans/" + plan_id + "/buckets?$select=id,name", what)) {
			buckets[Str(b, "id")] = Str(b, "name");
		}
		auto data = make_uniq<RowsBindData>();
		for (auto &t : All(client, "/v1.0/planner/plans/" + plan_id + "/tasks", what)) {
			int64_t percent = 0;
			auto pc = t.find("percentComplete");
			if (pc != t.end() && pc->second.is<int64_t>()) {
				percent = pc->second.get<int64_t>();
			}
			auto prio = t.find("priority");
			vector<Value> assignees;
			if (auto a = Obj(t, "assignments")) {
				for (auto &kv : *a) {
					assignees.emplace_back(kv.first); // keyed by user id
				}
			}
			const auto bucket_id = Str(t, "bucketId");
			data->rows.push_back(
			    {Text(plan_title), Value(plan_id), Text(buckets[bucket_id]), Text(bucket_id), Value(Str(t, "id")),
			     Text(Str(t, "title")),
			     Value(percent == 100 ? "completed" : percent == 0 ? "not started" : "in progress"),
			     Value::INTEGER(static_cast<int32_t>(percent)),
			     prio != t.end() && prio->second.is<int64_t>() ? Value::INTEGER(static_cast<int32_t>(prio->second.get<int64_t>()))
			                                                   : Value(LogicalType::INTEGER),
			     Time(Str(t, "startDateTime")), Time(Str(t, "dueDateTime")), Time(Str(t, "completedDateTime")),
			     Time(Str(t, "createdDateTime")), Value::LIST(LogicalType::VARCHAR, std::move(assignees))});
		}
		names = {"plan",   "plan_id", "bucket", "bucket_id", "id",        "title",   "status",
		         "percent_complete", "priority", "start", "due", "completed", "created", "assigned_to"};
		return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::VARCHAR,   LogicalType::INTEGER,   LogicalType::INTEGER,
		                LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP,
		                LogicalType::TIMESTAMP, LogicalType::LIST(LogicalType::VARCHAR)};
		result = std::move(data);
	});
	return result;
}

// --- To Do -----------------------------------------------------------------------
unique_ptr<FunctionData> TodoBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto list = Param(input, "list");
		const auto user = Param(input, "user");
		AuthContext auth;
		auto client = Client(context, auth);
		const string base = user.empty() ? "/v1.0/me" : "/v1.0/users/" + CheckedId(user, "user");
		const string what = "the To Do tasks of " + (user.empty() ? string("the signed-in user") : user);
		auto data = make_uniq<RowsBindData>();
		bool found = list.empty();
		for (auto &l : All(client, base + "/todo/lists?$select=id,displayName", what)) {
			const auto list_id = Str(l, "id");
			const auto list_name = Str(l, "displayName");
			if (!list.empty() && list != list_id && !StringUtil::CIEquals(list, list_name)) {
				continue;
			}
			found = true;
			for (auto &t : All(client, base + "/todo/lists/" + CheckedId(list_id, "list") + "/tasks", what)) {
				string due, completed, body;
				if (auto d = Obj(t, "dueDateTime")) {
					due = Str(*d, "dateTime");
				}
				if (auto c = Obj(t, "completedDateTime")) {
					completed = Str(*c, "dateTime");
				}
				if (auto b = Obj(t, "body")) {
					body = Str(*b, "content");
				}
				data->rows.push_back({Text(list_name), Value(list_id), Value(Str(t, "id")), Text(Str(t, "title")),
				                      Text(Str(t, "status")), Text(Str(t, "importance")), Time(due), Time(completed),
				                      Time(Str(t, "createdDateTime")), Time(Str(t, "lastModifiedDateTime")),
				                      Text(body)});
			}
		}
		if (!found) {
			throw InvalidInputException("sharepoint: no To Do list '%s' for %s", list,
			                            user.empty() ? string("the signed-in user") : user);
		}
		names = {"list", "list_id", "id", "title", "status", "importance", "due", "completed", "created", "modified",
		         "body"};
		return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::TIMESTAMP, LogicalType::TIMESTAMP,
		                LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::VARCHAR};
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

void RegisterTaskFunctions(ExtensionLoader &loader) {
	TableFunction planner("read_planner_tasks", {LogicalType::VARCHAR}, RowsScan, PlannerBind, CursorInit);
	duckdb_compat::NameParameters(planner, {"plan"});
	duckdb_compat::AddNamedParameter(planner, "group", LogicalType::VARCHAR);
	Register(loader, planner,
	         "The tasks of a Microsoft Planner plan, by its id, or by its name (with group := for app-only): bucket, "
	         "title, status, percent complete, priority, start/due/completed dates and the assigned user ids. Needs "
	         "Tasks.Read.All (application) or Tasks.Read (delegated).",
	         {"SELECT * FROM read_planner_tasks('Q1 launch', group := '<group id>')"});

	TableFunction todo("read_todo_tasks", {}, RowsScan, TodoBind, CursorInit);
	duckdb_compat::AddNamedParameter(todo, "list", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(todo, "user", LogicalType::VARCHAR);
	Register(loader, todo,
	         "The signed-in user's Microsoft To Do tasks (or user := someone's, app-only): list, title, status, "
	         "importance, due/completed dates and body. list := picks one list. Needs Tasks.Read (delegated) or "
	         "Tasks.Read.All (application).",
	         {"SELECT * FROM read_todo_tasks() WHERE status <> 'completed'"});
}

} // namespace sharepoint
} // namespace duckdb
