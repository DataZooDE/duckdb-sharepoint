// read_sharepoint_list_attachments(list, site :=, item_id :=) (#97): the
// files attached to a list's items. The list and its site are found through
// Graph as read_sharepoint_list finds them; the attachments themselves come
// from SharePoint's own REST API (Graph v1.0 has none), with a token for that
// host from the same secret (GetSharepointRestTokens). Content is fetched
// only when the content column is selected.
#include "sharepoint_attachments_functions.hpp"
#include "sharepoint_attachments.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_list_functions.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

constexpr idx_t kContentColumn = 3;

struct AttachmentsBindData : public TableFunctionData {
	SharepointSite site;
	shared_ptr<TokenSource> tokens;
	bool client_secret_app = false;
	vector<ListAttachment> attachments;
};

string JsonField(const string &body, const char *key, const string &what) {
	picojson::value v;
	if (!picojson::parse(v, body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: Graph's answer for %s is not JSON", what);
	}
	auto &o = v.get<picojson::object>();
	auto it = o.find(key);
	if (it == o.end() || !it->second.is<string>()) {
		throw IOException("sharepoint: Graph's answer for %s has no %s", what, key);
	}
	return it->second.get<string>();
}

//! SharePoint's answer to `path`, or an error that says what to do.
string RestGet(AttachmentsBindData &data, const string &path, const string &what) {
	string body;
	const int status = SharepointRestGet(*data.tokens, data.site.host, path, body);
	if (status < 200 || status >= 300) {
		throw IOException("sharepoint: %s: %s", what, ExplainSharepointRestError(status, body, data.client_secret_app));
	}
	return body;
}

unique_ptr<FunctionData> AttachmentsBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty() ||
		    StringValue::Get(input.inputs[0]) == "id:") {
			throw InvalidInputException("sharepoint: name a list, by its name or as id:<listId>");
		}
		const string list = StringValue::Get(input.inputs[0]);
		string site;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "site")) {
			if (!v->IsNull()) {
				site = StringValue::Get(*v);
			}
		}
		int64_t item_id = 0;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "item_id")) {
			if (!v->IsNull()) {
				item_id = v->GetValue<int64_t>();
				if (item_id < 1) {
					throw InvalidInputException("sharepoint: item_id must be a list item's id (1 or more)");
				}
			}
		}
		auto opener = ClientData::Get(context).file_opener.get();
		// As read_sharepoint_list: a named site needs no drive in the secret.
		auto auth = GetAuthContext(opener, SHAREPOINT_SCHEME, site.empty());
		GraphClient client(auth);
		const auto site_id = ResolveSiteId(client, auth, site);
		const string what = "list '" + list + "'" + (site.empty() ? string() : " on site '" + site + "'");
		const bool by_id = StringUtil::StartsWith(list, "id:");
		const auto list_id =
		    JsonField(client.GraphGet("/v1.0/sites/" + site_id + "/lists/" +
		                                  EncodeWorkbookName(by_id ? list.substr(3) : list) + "?$select=id",
		                              what),
		              "id", what);
		const auto web_url =
		    JsonField(client.GraphGet("/v1.0/sites/" + site_id + "?$select=webUrl", what), "webUrl", what);

		auto data = make_uniq<AttachmentsBindData>();
		if (!SplitSiteWebUrl(web_url, data->site)) {
			throw IOException("sharepoint: the site of %s is not on a SharePoint host ('%s')", what, web_url);
		}
		data->tokens = GetSharepointRestTokens(opener, SHAREPOINT_SCHEME, data->site.host, data->client_secret_app);
		string path = AttachmentItemsPath(data->site, list_id, item_id);
		if (path.empty()) {
			throw IOException("sharepoint: %s has an id that is not a GUID ('%s')", what, list_id);
		}
		const string attachments_of = "the attachments of " + what;
		while (!path.empty()) {
			string next, error;
			if (!ParseAttachmentPage(RestGet(*data, path, attachments_of), data->site, item_id, data->attachments, next,
			                         error)) {
				throw IOException("sharepoint: %s: %s", attachments_of, error);
			}
			path = next;
		}
		names = {"item_id", "file_name", "url", "content"};
		return_types = {LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BLOB};
		result = std::move(data);
	});
	return result;
}

struct AttachmentsState : public GlobalTableFunctionState {
	idx_t cursor = 0;
	vector<column_t> column_ids;
};

unique_ptr<GlobalTableFunctionState> AttachmentsInit(ClientContext &, TableFunctionInitInput &input) {
	auto state = make_uniq<AttachmentsState>();
	state->column_ids = input.column_ids;
	return std::move(state);
}

void AttachmentsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = const_cast<AttachmentsBindData &>(input.bind_data->Cast<AttachmentsBindData>());
	auto &state = input.global_state->Cast<AttachmentsState>();
	idx_t out = 0;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		// A row's content is one request: a small chunk keeps a LIMIT cheap.
		while (state.cursor < data.attachments.size() && out < 64) {
			auto &a = data.attachments[state.cursor];
			for (idx_t c = 0; c < state.column_ids.size(); c++) {
				switch (state.column_ids[c]) {
				case 0:
					output.SetValue(c, out, Value::BIGINT(a.item_id));
					break;
				case 1:
					output.SetValue(c, out, Value(a.file_name));
					break;
				case 2:
					output.SetValue(c, out, Value(AttachmentWebUrl(data.site, a.server_relative_url)));
					break;
				case kContentColumn: {
					const auto bytes = RestGet(data, AttachmentContentPath(data.site, a.server_relative_url),
					                           "attachment '" + a.file_name + "' of item " + std::to_string(a.item_id));
					output.SetValue(c, out, Value::BLOB(const_data_ptr_cast(bytes.data()), bytes.size()));
					break;
				}
				default:
					output.SetValue(c, out, Value(output.data[c].GetType()));
				}
			}
			state.cursor++;
			out++;
		}
	});
	duckdb_compat::SetChildCardinality(output, out);
}

} // namespace

void RegisterAttachmentFunctions(ExtensionLoader &loader) {
	TableFunction fn("read_sharepoint_list_attachments", {LogicalType::VARCHAR}, AttachmentsScan, AttachmentsBind,
	                 AttachmentsInit);
	duckdb_compat::NameParameters(fn, {"list"});
	duckdb_compat::AddNamedParameter(fn, "site", LogicalType::VARCHAR);
	duckdb_compat::AddNamedParameter(fn, "item_id", LogicalType::BIGINT);
	fn.projection_pushdown = true;
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "The files attached to a SharePoint list's items: item_id, file_name, url and content (BLOB, downloaded only "
	    "when selected). Uses SharePoint's own API (Microsoft Graph has none for attachments), which needs a "
	    "certificate or a user sign-in -- not a client secret -- and a SharePoint API permission.";
	desc.examples = {"SELECT item_id, file_name FROM read_sharepoint_list_attachments('Orders', site := 'Sales')",
	                 "SELECT content FROM read_sharepoint_list_attachments('Orders', item_id := 7)"};
	desc.categories = {"sharepoint", "list"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
