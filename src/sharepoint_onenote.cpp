// read_onenote_pages() (#102): the pages of a site's OneNote notebooks, with
// their section and change times -- page-level tracking that the filesystem
// (a section is one .one file) cannot give. OneNote has no delta: track with
// WHERE modified > <last run>. content := true adds each page's HTML (#105).
//
// COPY ... TO '<notebook>/<section>' (FORMAT onenote) (#105): one new page
// per row, or, for a row with an id, its content appended to that page.
//
// Microsoft restricts app-only OneNote access, so both are for a signed-in
// user (Notes.Read / Notes.ReadWrite).
#include "sharepoint_onenote.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_copy_rewrite.hpp"
#include "sharepoint_onenote_page.hpp"
#include "sharepoint_list_functions.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_weburl.hpp"
#include "sharepoint_list.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <cstdint>

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

//! The hint for a refused OneNote call: what usually causes it.
[[noreturn]] void ThrowRefused(const string &what, const string &text, bool write) {
	throw InvalidInputException("sharepoint: %s %s was refused. Microsoft restricts app-only access to OneNote: use a "
	                            "signed-in user (PROVIDER device_code or authorization_code) with %s in GRAPH_SCOPE, "
	                            "e.g. GRAPH_SCOPE 'https://graph.microsoft.com/%s Files.ReadWrite.All "
	                            "Sites.ReadWrite.All offline_access'. (%s)",
	                            write ? "writing" : "reading", what, write ? "Notes.ReadWrite" : "Notes.Read",
	                            write ? "Notes.ReadWrite" : "Notes.Read", text);
}

bool IsRefusal(const string &text) {
	return text.find("HTTP 401") != string::npos || text.find("HTTP 403") != string::npos ||
	       text.find("forbidden") != string::npos;
}

//! A OneNote collection: on 401/403 say what usually causes it.
vector<picojson::object> OneNote(GraphClient &client, const string &path, const string &what) {
	try {
		return All(client, path, what);
	} catch (std::exception &e) {
		if (IsRefusal(e.what())) {
			ThrowRefused(what, e.what(), false);
		}
		throw;
	}
}

//! A OneNote GET of one resource (a page's content), refusals explained.
string OneNoteGet(GraphClient &client, const string &path, const string &what) {
	try {
		return client.GraphGet(path, what);
	} catch (std::exception &e) {
		if (IsRefusal(e.what())) {
			ThrowRefused(what, e.what(), false);
		}
		throw;
	}
}

//! A OneNote POST or PATCH, refusals explained. Never re-sent but after
//! throttling: a page created twice is a duplicate page.
string OneNoteSend(GraphClient &client, const string &method, const string &path, const string &body,
                   const string &what, const string &content_type) {
	try {
		return client.GraphSend(method, path, body, what, false, content_type);
	} catch (std::exception &e) {
		if (IsRefusal(e.what())) {
			ThrowRefused(what, e.what(), true);
		}
		throw;
	}
}

//! Graph ids are spliced into URLs: only their alphabet passes.
string CheckedPageId(const string &id) {
	for (char c : id) {
		if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '!')) {
			throw InvalidInputException("sharepoint: '%s' is not a OneNote page id", id);
		}
	}
	return id;
}

//! "/v1.0/sites/{id}/onenote" for `site` (default: the secret's).
string OneNoteBase(GraphClient &client, const AuthContext &auth, const string &site) {
	return "/v1.0/sites/" + ResolveSiteId(client, auth, site) + "/onenote";
}

unique_ptr<FunctionData> PagesBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto site = Param(input, "site");
		const auto notebook = Param(input, "notebook");
		const auto section = Param(input, "section");
		bool with_content = false;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "content")) {
			with_content = !v->IsNull() && BooleanValue::Get(*v);
		}
		auto opener = ClientData::Get(context).file_opener.get();
		auto auth = GetAuthContext(opener, SHAREPOINT_SCHEME, site.empty());
		GraphClient client(auth);
		const string base = OneNoteBase(client, auth, site);
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
					const auto page_id = Str(page, "id");
					data->rows.push_back(
					    {Value(nb_name), Value(sec_name), Text(Str(page, "title")),
					     ParseIsoTimestamp(Str(page, "createdDateTime"), created) ? Value::TIMESTAMP(timestamp_t(created))
					                                                              : Value(LogicalType::TIMESTAMP),
					     ParseIsoTimestamp(Str(page, "lastModifiedDateTime"), modified)
					         ? Value::TIMESTAMP(timestamp_t(modified))
					         : Value(LogicalType::TIMESTAMP),
					     Text(web_url), Text(page_id)});
					if (with_content) {
						data->rows.back().push_back(
						    page_id.empty() ? Value(LogicalType::VARCHAR)
						                    : Value(OneNoteGet(client, base + "/pages/" + CheckedPageId(page_id) + "/content",
						                                       "the content of page '" + Str(page, "title") + "'")));
					}
				}
			}
		}
		if (!notebook_found) {
			throw InvalidInputException("sharepoint: the site has no OneNote notebook '%s'", notebook);
		}
		names = {"notebook", "section", "title", "created", "modified", "web_url", "id"};
		return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP,
		                LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR};
		if (with_content) {
			names.push_back("content");
			return_types.push_back(LogicalType::VARCHAR);
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

// ===========================================================================
// COPY (SELECT title, content [, id] ...) TO '<notebook>/<section>'
//     (FORMAT onenote [, SITE 'Sales'])
//
// A row without an id (no id column, or a NULL id) creates a page in the
// section from its title and content (the body's HTML); a row with one
// appends its content to the end of that page. A missing section is created;
// the notebook must exist. The target is a section, not a file: the same
// in-place rewrite as the list and Excel writers (sharepoint_copy_rewrite.hpp).
// ===========================================================================

constexpr const char *kOneNoteFormat = "onenote";
constexpr const char *kOneNoteFormatInternal = "__sharepoint_onenote";

struct OneNoteWriteBindData : public FunctionData {
	OneNoteTarget target;
	string site;
	OneNoteWritePlan plan;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<OneNoteWriteBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<OneNoteWriteBindData>();
		return target.notebook == other.target.notebook && target.section == other.target.section &&
		       site == other.site && plan.title == other.plan.title && plan.content == other.plan.content &&
		       plan.id == other.plan.id;
	}
};

unique_ptr<FunctionData> OneNoteWriteBind(ClientContext &, CopyFunctionBindInput &input,
                                          const duckdb_compat::CopyBindNames &names, const vector<LogicalType> &) {
	auto &info = input.info;
	if (!IsRewrittenInPlace(info)) {
		throw InvalidInputException("sharepoint: write OneNote pages with FORMAT %s", kOneNoteFormat);
	}
	auto data = make_uniq<OneNoteWriteBindData>();
	string error;
	if (!ParseOneNoteTarget(info.file_path, data->target, error)) {
		throw InvalidInputException("sharepoint: %s", error);
	}
	if (auto site = FindCopyOption(info.options, "site")) {
		if (site->size() == 1 && !(*site)[0].IsNull()) {
			data->site = (*site)[0].ToString();
		}
	}
	vector<string> columns;
	for (auto &n : names) {
		columns.push_back(duckdb_compat::Str(n));
	}
	if (!PlanOneNoteWrite(columns, data->plan, error)) {
		throw InvalidInputException("sharepoint: %s", error);
	}
	return std::move(data);
}

struct OneNoteWriteGlobalState : public GlobalFunctionData {
	AuthContext auth;
	string base;
	string section_id;
	string what; // "section 'Q4' of notebook 'Projects'"
};

unique_ptr<GlobalFunctionData> OneNoteWriteInitGlobal(ClientContext &context, FunctionData &bind_p, const string &) {
	auto &bind = bind_p.Cast<OneNoteWriteBindData>();
	auto g = make_uniq<OneNoteWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto opener = ClientData::Get(context).file_opener.get();
		g->auth = GetAuthContext(opener, SHAREPOINT_SCHEME, bind.site.empty());
		GraphClient client(g->auth);
		g->base = OneNoteBase(client, g->auth, bind.site);
		const auto &t = bind.target;
		g->what = "section '" + t.section + "' of notebook '" + t.notebook + "'";
		string notebook_id;
		vector<string> notebooks;
		for (auto &nb : OneNote(client, g->base + "/notebooks?$select=id,displayName", "the site's OneNote notebooks")) {
			notebooks.push_back(Str(nb, "displayName"));
			if (notebook_id.empty() && StringUtil::CIEquals(notebooks.back(), t.notebook)) {
				notebook_id = Str(nb, "id");
			}
		}
		if (notebook_id.empty()) {
			throw InvalidInputException("sharepoint: the site has no OneNote notebook '%s' (it has: %s)", t.notebook,
			                            notebooks.empty() ? string("none") : StringUtil::Join(notebooks, ", "));
		}
		const string sections = g->base + "/notebooks/" + CheckedPageId(notebook_id) + "/sections";
		for (auto &sec : OneNote(client, sections + "?$select=id,displayName",
		                         "the sections of notebook '" + t.notebook + "'")) {
			if (StringUtil::CIEquals(Str(sec, "displayName"), t.section)) {
				g->section_id = Str(sec, "id");
				break;
			}
		}
		if (g->section_id.empty()) {
			picojson::object body;
			body["displayName"] = picojson::value(t.section);
			picojson::value created;
			const auto reply =
			    OneNoteSend(client, "POST", sections, picojson::value(body).serialize(), g->what, "application/json");
			if (!picojson::parse(created, reply).empty() || !created.is<picojson::object>() ||
			    Str(created.get<picojson::object>(), "id").empty()) {
				throw IOException("sharepoint: creating %s returned no section id", g->what);
			}
			g->section_id = Str(created.get<picojson::object>(), "id");
		}
		CheckedPageId(g->section_id);
	});
	return std::move(g);
}

unique_ptr<LocalFunctionData> OneNoteWriteInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

string CellText(DataChunk &input, int column, idx_t row) {
	if (column < 0) {
		return string();
	}
	auto v = input.GetValue(static_cast<idx_t>(column), row);
	return v.IsNull() ? string() : v.ToString();
}

void OneNoteWriteSink(ExecutionContext &, FunctionData &bind_p, GlobalFunctionData &gstate_p, LocalFunctionData &,
                      DataChunk &input) {
	auto &bind = bind_p.Cast<OneNoteWriteBindData>();
	auto &g = gstate_p.Cast<OneNoteWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		GraphClient client(g.auth);
		for (idx_t r = 0; r < input.size(); r++) {
			const auto id = CellText(input, bind.plan.id, r);
			const auto content = CellText(input, bind.plan.content, r);
			if (!id.empty()) {
				if (content.empty()) {
					continue; // nothing to append
				}
				OneNoteSend(client, "PATCH", g.base + "/pages/" + CheckedPageId(id) + "/content",
				            OneNoteAppendCommands(content), "page " + id + " in " + g.what, "application/json");
				continue;
			}
			string document, error;
			const auto title = CellText(input, bind.plan.title, r);
			if (!OneNotePageDocument(title, content, document, error)) {
				throw InvalidInputException("sharepoint: %s", error);
			}
			OneNoteSend(client, "POST", g.base + "/sections/" + g.section_id + "/pages", document,
			            "a page '" + title + "' in " + g.what, "text/html");
		}
	});
}

void OneNoteWriteCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

void OneNoteWriteFinalize(ClientContext &, FunctionData &, GlobalFunctionData &) {
}

CopyFunctionExecutionMode OneNoteWriteExecutionMode(bool, bool) {
	// One writer, rows in query order: pages are created in it.
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

void OneNoteCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, kInPlaceMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, "site", LogicalType::VARCHAR);
}

BoundStatement OneNoteCopyPlan(Binder &binder, CopyStatement &stmt) {
	return RewriteCopyInPlace(binder, stmt, kOneNoteFormatInternal, "OneNote section");
}

} // namespace

void RegisterOneNoteFunctions(ExtensionLoader &loader) {
	TableFunction fn("read_onenote_pages", {}, RowsScan, PagesBind, CursorInit);
	duckdb_compat::AddNamedParameter(fn, "site", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "notebook", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "section", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "content", LogicalType::BOOLEAN);
	Register(loader, fn,
	         "The pages of a site's OneNote notebooks (default the secret's site; notebook := and section := narrow "
	         "it): notebook, section, title, created and modified times, web address and id; content := true adds "
	         "each page's HTML (one request per page). OneNote has no delta: track changes with WHERE modified > "
	         "<last run>. Needs a signed-in user with Notes.Read (Microsoft restricts app-only OneNote access). "
	         "Write pages with COPY ... TO '<notebook>/<section>' (FORMAT onenote).",
	         {"SELECT section, title, modified FROM read_onenote_pages(notebook := 'Projects') ORDER BY modified DESC",
	          "SELECT title, content FROM read_onenote_pages(notebook := 'Projects', section := 'Q4', content := true)"});

	CopyFunction front(duckdb_compat::FunctionName(kOneNoteFormat));
	front.plan = OneNoteCopyPlan;
	loader.RegisterFunction(front);

	CopyFunction writer(duckdb_compat::FunctionName(kOneNoteFormatInternal));
	writer.copy_options = OneNoteCopyOptions;
	writer.copy_to_bind = OneNoteWriteBind;
	writer.copy_to_initialize_global = OneNoteWriteInitGlobal;
	writer.copy_to_initialize_local = OneNoteWriteInitLocal;
	writer.copy_to_sink = OneNoteWriteSink;
	writer.copy_to_combine = OneNoteWriteCombine;
	writer.copy_to_finalize = OneNoteWriteFinalize;
	writer.execution_mode = OneNoteWriteExecutionMode;
	loader.RegisterFunction(writer);
}

} // namespace sharepoint
} // namespace duckdb
