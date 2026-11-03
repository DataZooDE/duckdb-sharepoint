// sharepoint_changes(path, token := ...) (#62): the files added, changed and
// deleted in the secret's drive since a token -- Graph's delta query
// (GET /drives/{d}/root/delta). Stateless: the caller keeps the token, which
// the last row (_change = 'checkpoint') carries, changes or not.
#include "sharepoint_changes.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_delta.hpp"
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

//! A nextLink chain longer than this is a server bug, not a drive.
constexpr int kMaxDeltaPages = 100000;

string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
}

struct ChangesBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

unique_ptr<FunctionData> ChangesBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the path must not be NULL");
		}
		const auto arg = StringValue::Get(input.inputs[0]);
		const string path = IsSharepointUri(arg) ? arg : string(SHAREPOINT_SCHEME) + arg;
		auto parsed = ParseSharepointUri(path);
		if (!parsed.ok || parsed.uri.kind != SharepointUriKind::PATH) {
			throw InvalidInputException("sharepoint: sharepoint_changes takes a folder path, e.g. 'sharepoint://Finance/'");
		}
		const auto folder = parsed.uri.RootRelativePath();
		string token;
		if (auto v = duckdb_compat::FindNamedParameter(input.named_parameters, "token")) {
			if (!v->IsNull()) {
				token = StringValue::Get(*v);
			}
		}
		for (char c : token) {
			// Sent back in a URL as Graph wrote it; nothing that could change which.
			if (!(isalnum(static_cast<unsigned char>(c)) || c == '%' || c == '-' || c == '_' || c == '.' || c == '~' ||
			      c == '=')) {
				throw InvalidInputException("sharepoint: that is not a token sharepoint_changes returned");
			}
		}
		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, path);
		GraphClient client(auth);
		const string what = "the changes under '" + path + "'";

		string next = "/v1.0/drives/" + auth.drive_id +
		              "/root/delta?$select=id,name,size,file,folder,deleted,root,parentReference,lastModifiedDateTime" +
		              (token.empty() ? string() : "&token=" + token);
		string next_token;
		auto data = make_uniq<ChangesBindData>();
		std::unordered_map<string, idx_t> row_of; // an item seen twice: its last state wins
		for (int pages = 0; !next.empty(); pages++) {
			if (pages >= kMaxDeltaPages) {
				throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxDeltaPages);
			}
			picojson::value holder;
			try {
				if (!picojson::parse(holder, client.GraphGet(next, what)).empty() || !holder.is<picojson::object>()) {
					throw IOException("sharepoint: unexpected response reading %s", what);
				}
			} catch (std::exception &e) {
				const string text = e.what();
				if (text.find("HTTP 410") != string::npos || text.find("resyncRequired") != string::npos) {
					throw InvalidInputException("sharepoint: the token for %s has expired; start over with token := NULL "
					                            "(a full listing) or 'latest' (from now on)",
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
					if (item.find("root") != item.end()) {
						continue; // the drive's root folder itself
					}
					const bool deleted = item.find("deleted") != item.end();
					string parent_path;
					auto parent = item.find("parentReference");
					if (parent != item.end() && parent->second.is<picojson::object>()) {
						parent_path = Str(parent->second.get<picojson::object>(), "path");
					}
					const auto name = Str(item, "name");
					const auto rel = DeltaItemPath(parent_path, name);
					// A delete often comes without a path: reported wherever it was.
					if (!rel.empty() && !PathUnder(rel, folder)) {
						continue;
					}
					if (rel.empty() && !deleted) {
						continue;
					}
					int64_t micros;
					auto size = item.find("size");
					vector<Value> row = {
					    Value(deleted ? "delete" : "upsert"),
					    rel.empty() ? Value(LogicalType::VARCHAR) : Value(string(SHAREPOINT_SCHEME) + rel),
					    Value(Str(item, "id")),
					    name.empty() ? Value(LogicalType::VARCHAR) : Value(name),
					    Value::BOOLEAN(item.find("folder") != item.end()),
					    !deleted && size != item.end() && size->second.is<int64_t>()
					        ? Value::BIGINT(size->second.get<int64_t>())
					        : Value(LogicalType::BIGINT),
					    ParseIsoTimestamp(Str(item, "lastModifiedDateTime"), micros) ? Value::TIMESTAMP(timestamp_t(micros))
					                                                                 : Value(LogicalType::TIMESTAMP),
					    Value(LogicalType::VARCHAR)};
					const auto id = Str(item, "id");
					auto seen = row_of.find(id);
					if (seen != row_of.end()) {
						data->rows[seen->second] = std::move(row);
					} else {
						row_of[id] = data->rows.size();
						data->rows.push_back(std::move(row));
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
		data->rows.push_back({Value("checkpoint"), Value(LogicalType::VARCHAR), Value(LogicalType::VARCHAR),
		                      Value(LogicalType::VARCHAR), Value(LogicalType::BOOLEAN), Value(LogicalType::BIGINT),
		                      Value(LogicalType::TIMESTAMP), Value(next_token)});
		names = {"_change", "path", "id", "name", "is_folder", "size", "modified", "token"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR,
		                LogicalType::BOOLEAN, LogicalType::BIGINT,  LogicalType::TIMESTAMP, LogicalType::VARCHAR};
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

void ChangesScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ChangesBindData>();
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

void RegisterChangeFunctions(ExtensionLoader &loader) {
	TableFunction fn("sharepoint_changes", {LogicalType::VARCHAR}, ChangesScan, ChangesBind, CursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	duckdb_compat::AddNamedParameter(fn, "token", LogicalType::VARCHAR);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "What changed under a sharepoint:// folder since a token (Graph's delta query): one row per file or folder "
	    "added or changed (_change 'upsert') or deleted ('delete'), then one 'checkpoint' row whose token column is "
	    "the token for next time. Without a token: everything (a full listing); token := 'latest': nothing, just a "
	    "token from now on.";
	desc.examples = {"SELECT * FROM sharepoint_changes('sharepoint://Finance/')",
	                 "SELECT * FROM sharepoint_changes('sharepoint://Finance/', token := 'aTE09...')"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
