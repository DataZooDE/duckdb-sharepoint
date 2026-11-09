// read_outlook_messages() (#126): a mailbox's Outlook messages as a table,
// over the same secret and Graph client as the rest of the extension:
//   /me (or /users/{u})/[mailFolders/{f}/]messages, newest first
// Pages are fetched as the scan goes, not at bind: a mailbox can hold far
// more messages than a list holds items, and a LIMIT stops the paging. Needs
// Mail.Read (delegated, or application -- then user := is required, and an
// admin should limit the app to chosen mailboxes).
#include "sharepoint_mail_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_mail.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
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

constexpr int kMaxFolderPages = 100;
constexpr const char *kTextBody = "outlook.body-content-type=\"text\"";

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

Value Bool(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<bool>()) ? Value::BOOLEAN(it->second.get<bool>())
	                                                : Value(LogicalType::BOOLEAN);
}

Value Time(const string &iso) {
	int64_t us;
	return !iso.empty() && ParseIsoTimestamp(iso, us) ? Value::TIMESTAMP(timestamp_t(us))
	                                                  : Value(LogicalType::TIMESTAMP);
}

//! The addresses of a recipients array ([{emailAddress: {name, address}}]).
Value Addresses(const picojson::object &o, const char *key) {
	vector<Value> out;
	auto it = o.find(key);
	if (it != o.end() && it->second.is<picojson::array>()) {
		for (auto &r : it->second.get<picojson::array>()) {
			if (r.is<picojson::object>()) {
				if (auto email = Obj(r.get<picojson::object>(), "emailAddress")) {
					out.emplace_back(Str(*email, "address"));
				}
			}
		}
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(out));
}

Value Strings(const picojson::object &o, const char *key) {
	vector<Value> out;
	auto it = o.find(key);
	if (it != o.end() && it->second.is<picojson::array>()) {
		for (auto &v : it->second.get<picojson::array>()) {
			if (v.is<string>()) {
				out.emplace_back(v.get<string>());
			}
		}
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(out));
}

string Param(TableFunctionBindInput &input, const char *name) {
	if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, name)) {
		if (!v->IsNull()) {
			return StringValue::Get(*v);
		}
	}
	return string();
}

//! GET a mailbox resource; the refusals people hit get their cause named.
picojson::object MailGet(GraphClient &client, const string &path, const string &what, bool text_body) {
	string body;
	try {
		body = client.GraphGet(path, what, nullptr, text_body ? kTextBody : "");
	} catch (std::exception &e) {
		const string text = e.what();
		if (text.find("/me request is only valid with delegated") != string::npos) {
			throw InvalidInputException("sharepoint: reading %s: an app-only secret has no mailbox of its own; name "
			                            "one with user := '<user@domain>' (%s)",
			                            what, text);
		}
		if (text.find("MailboxNotEnabledForRESTAPI") != string::npos ||
		    text.find("MailboxNotHostedInExchangeOnline") != string::npos) {
			throw InvalidInputException("sharepoint: reading %s: the user has no Exchange Online mailbox (%s)", what,
			                            text);
		}
		if (text.find("HTTP 401") != string::npos || text.find("HTTP 403") != string::npos ||
		    text.find("ErrorAccessDenied") != string::npos) {
			throw InvalidInputException(
			    "sharepoint: reading %s was refused. It needs Mail.Read: as an application permission with admin "
			    "consent (and, if an application access policy limits the app's mailboxes, this mailbox in it), or "
			    "for a signed-in user in GRAPH_SCOPE, e.g. 'https://graph.microsoft.com/Mail.Read "
			    "https://graph.microsoft.com/Files.ReadWrite.All offline_access'. (%s)",
			    what, text);
		}
		throw;
	}
	picojson::value holder;
	if (!picojson::parse(holder, body).empty() || !holder.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected response reading %s", what);
	}
	return holder.get<picojson::object>();
}

string NextPath(const picojson::object &page) {
	auto link = page.find("@odata.nextLink");
	return (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>()) : string();
}

struct MailBindData : public TableFunctionData {
	AuthContext auth;
	string first_path;
	string what;
	bool body = false;
};

unique_ptr<FunctionData> MailBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto data = make_uniq<MailBindData>();
		const auto user = Param(input, "user");
		const auto folder = Param(input, "folder");
		MailQuery q;
		q.mailbox = MailboxBase(user);
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "since")) {
			if (!v->IsNull()) {
				const auto ts = v->DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
				if (Value::IsFinite(ts)) {
					q.has_since = true;
					q.since_micros = Timestamp::GetEpochMicroSeconds(ts);
				}
			}
		}
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "body")) {
			data->body = !v->IsNull() && BooleanValue::Get(*v);
		}
		q.body = data->body;
		data->what = "the mail of " + (user.empty() ? string("the signed-in user") : user);
		auto opener = ClientData::Get(context).file_opener.get();
		data->auth = GetAuthContext(opener, SHAREPOINT_SCHEME, false);
		if (!folder.empty() && !WellKnownMailFolder(folder, q.folder)) {
			// A display name: one of the mailbox's top-level folders.
			GraphClient client(data->auth);
			vector<string> seen;
			string next = q.mailbox + "/mailFolders?$select=id,displayName&$top=100";
			for (int pages = 0; !next.empty() && q.folder.empty() && pages < kMaxFolderPages; pages++) {
				const auto page = MailGet(client, next, data->what, false);
				auto values = page.find("value");
				if (values != page.end() && values->second.is<picojson::array>()) {
					for (auto &f : values->second.get<picojson::array>()) {
						if (!f.is<picojson::object>()) {
							continue;
						}
						const auto &obj = f.get<picojson::object>();
						seen.push_back(Str(obj, "displayName"));
						if (StringUtil::CIEquals(seen.back(), folder) || Str(obj, "id") == folder) {
							q.folder = Str(obj, "id");
							break;
						}
					}
				}
				next = NextPath(page);
			}
			if (q.folder.empty()) {
				throw InvalidInputException("sharepoint: %s has no top-level mail folder '%s' (it has: %s)",
				                            data->what, folder, StringUtil::Join(seen, ", "));
			}
			data->what += ", folder '" + folder + "'";
		} else if (!folder.empty()) {
			data->what += ", folder '" + folder + "'";
		}
		data->first_path = MailMessagesPath(q);
		names = {"id",       "received",        "sent",       "subject",      "from_name",
		         "from_email", "to_emails",     "cc_emails",  "is_read",      "is_draft",
		         "has_attachments", "importance", "categories", "conversation_id", "internet_message_id",
		         "body_preview", "web_url"};
		const auto list = LogicalType::LIST(LogicalType::VARCHAR);
		return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR,   list,                   list,
		                LogicalType::BOOLEAN, LogicalType::BOOLEAN,   LogicalType::BOOLEAN,   LogicalType::VARCHAR,
		                list,                 LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::VARCHAR};
		if (data->body) {
			names.push_back("body");
			return_types.push_back(LogicalType::VARCHAR);
		}
		result = std::move(data);
	});
	return result;
}

struct MailScanState : public GlobalTableFunctionState {
	string next;
	vector<vector<Value>> rows;
	idx_t cursor = 0;

	idx_t MaxThreads() const override {
		return 1; // one page chain
	}
};

unique_ptr<GlobalTableFunctionState> MailInit(ClientContext &, TableFunctionInitInput &input) {
	auto state = make_uniq<MailScanState>();
	state->next = input.bind_data->Cast<MailBindData>().first_path;
	return std::move(state);
}

vector<Value> MessageRow(const picojson::object &m, bool with_body) {
	string from_name, from_email, body;
	if (auto from = Obj(m, "from")) {
		if (auto email = Obj(*from, "emailAddress")) {
			from_name = Str(*email, "name");
			from_email = Str(*email, "address");
		}
	}
	vector<Value> row = {Text(Str(m, "id")),
	                     Time(Str(m, "receivedDateTime")),
	                     Time(Str(m, "sentDateTime")),
	                     Text(Str(m, "subject")),
	                     Text(from_name),
	                     Text(from_email),
	                     Addresses(m, "toRecipients"),
	                     Addresses(m, "ccRecipients"),
	                     Bool(m, "isRead"),
	                     Bool(m, "isDraft"),
	                     Bool(m, "hasAttachments"),
	                     Text(Str(m, "importance")),
	                     Strings(m, "categories"),
	                     Text(Str(m, "conversationId")),
	                     Text(Str(m, "internetMessageId")),
	                     Text(Str(m, "bodyPreview")),
	                     Text(Str(m, "webLink"))};
	if (with_body) {
		if (auto b = Obj(m, "body")) {
			body = Str(*b, "content");
		}
		row.push_back(Text(body));
	}
	return row;
}

void MailScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<MailBindData>();
	auto &state = input.global_state->Cast<MailScanState>();
	idx_t out = 0;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		while (out < STANDARD_VECTOR_SIZE) {
			if (state.cursor == state.rows.size()) {
				if (state.next.empty()) {
					break;
				}
				GraphClient client(bind.auth);
				const auto page = MailGet(client, state.next, bind.what, bind.body);
				state.rows.clear();
				state.cursor = 0;
				auto values = page.find("value");
				if (values != page.end() && values->second.is<picojson::array>()) {
					for (auto &m : values->second.get<picojson::array>()) {
						if (m.is<picojson::object>()) {
							state.rows.push_back(MessageRow(m.get<picojson::object>(), bind.body));
						}
					}
				}
				state.next = NextPath(page);
				continue;
			}
			auto &row = state.rows[state.cursor++];
			for (idx_t c = 0; c < row.size(); c++) {
				output.SetValue(c, out, row[c]);
			}
			out++;
		}
	});
	duckdb_compat::SetChildCardinality(output, out);
}

} // namespace

void RegisterMailFunctions(ExtensionLoader &loader) {
	TableFunction fn("read_outlook_messages", {}, MailScan, MailBind, MailInit);
	duckdb_compat::AddNamedParameter(fn, "user", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "folder", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "since", LogicalType::TIMESTAMP);
	duckdb_compat::AddNamedParameter(fn, "body", LogicalType::BOOLEAN);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "The signed-in user's Outlook messages (or user := someone's, app-only), newest first: received and sent "
	    "times, subject, sender, recipients, flags, categories, conversation id, preview and web address. folder := "
	    "a well-known folder (inbox, sentitems, drafts, archive, ...) or a top-level folder's name; since := only "
	    "messages received from then on; body := true adds the body as text. Needs Mail.Read (delegated or "
	    "application).";
	desc.examples = {"SELECT received, from_email, subject FROM read_outlook_messages(folder := 'inbox', since := "
	                 "TIMESTAMP '2026-11-01')",
	                 "SELECT * FROM read_outlook_messages(user := 'invoices@contoso.com', folder := 'inbox') LIMIT 50"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
