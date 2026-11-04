// sharepoint_permissions(path) (#98): who can reach an item -- direct grants,
// what is inherited from the site, and sharing links with their scope -- one
// row per grantee. A folder or file by its path, or files by a glob.
// The parsing is pure (sharepoint_permissions.*); the listing is here.
#include "sharepoint_permissions_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_glob.hpp"
#include "sharepoint_list.hpp"
#include "sharepoint_permissions.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

constexpr int kMaxPermissionPages = 1000;

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

Value Text(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

unique_ptr<FunctionData> PermissionsBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("sharepoint: the path must not be NULL");
		}
		const auto arg = StringValue::Get(input.inputs[0]);
		const string path = IsSharepointUri(arg) ? arg : string(SHAREPOINT_SCHEME) + arg;
		auto parsed = ParseSharepointUri(path);
		if (!parsed.ok) {
			throw InvalidInputException("sharepoint: %s", parsed.error);
		}
		const auto &uri = parsed.uri;
		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, path);
		GraphClient client(auth);

		// The items: one by id or literal path (a folder too), or the files a
		// glob matches.
		vector<std::pair<string, string>> items; // (sharepoint:// path, item id)
		bool glob = false;
		for (auto &segment : uri.segments) {
			glob = glob || HasGlobMetacharacters(segment);
		}
		if (uri.kind == SharepointUriKind::FILE_ID) {
			const auto meta = client.StatById(uri.item_id);
			if (!meta.found) {
				throw InvalidInputException("sharepoint: no such item: '%s'", path);
			}
			items.emplace_back(path, meta.id);
		} else if (!glob) {
			const auto meta = client.StatByPath(uri.RootRelativePath());
			if (!meta.found) {
				throw InvalidInputException("sharepoint: no such file or folder: '%s'", path);
			}
			items.emplace_back(path, meta.id);
		} else {
			auto lister = [&client](const string &folder) -> vector<GlobEntry> {
				vector<GlobEntry> entries;
				for (auto &c : client.ListChildren(folder)) {
					entries.push_back(GlobEntry {c.name, c.is_folder});
				}
				return entries;
			};
			for (auto &pattern : ExpandBraces(uri.RootRelativePath())) {
				auto expanded = ParseSharepointUri(string(SHAREPOINT_SCHEME) + pattern);
				if (!expanded.ok || expanded.uri.segments.empty()) {
					continue;
				}
				for (auto &rel : sharepoint::GlobFiles(expanded.uri.segments, lister)) {
					const auto meta = client.StatByPath(rel);
					if (meta.found) {
						items.emplace_back(string(SHAREPOINT_SCHEME) + rel, meta.id);
					}
				}
			}
		}

		auto data = make_uniq<RowsBindData>();
		for (auto &item : items) {
			const string what = "the permissions of '" + item.first + "'";
			string next = "/v1.0/drives/" + auth.drive_id + "/items/" + item.second + "/permissions";
			for (int pages = 0; !next.empty(); pages++) {
				if (pages >= kMaxPermissionPages) {
					throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxPermissionPages);
				}
				vector<PermissionRow> rows;
				string next_link, error;
				if (!ParsePermissions(client.GraphGet(next, what), rows, next_link, error)) {
					throw IOException("sharepoint: unexpected response reading %s: %s", what, error);
				}
				for (auto &r : rows) {
					vector<Value> roles;
					for (auto &role : r.roles) {
						roles.emplace_back(role);
					}
					int64_t micros;
					data->rows.push_back({Value(item.first), Value(item.second), Text(r.permission_id),
					                      Value::LIST(LogicalType::VARCHAR, roles), Value(r.granted_by),
					                      Text(r.grantee_type), Text(r.grantee), Text(r.grantee_email),
					                      Text(r.link_type), Text(r.link_scope), Text(r.link_url),
					                      Text(r.inherited_from),
					                      !r.expires.empty() && ParseIsoTimestamp(r.expires, micros)
					                          ? Value::TIMESTAMP(timestamp_t(micros))
					                          : Value(LogicalType::TIMESTAMP)});
				}
				next = next_link.empty() ? string() : GraphPathOfLink(next_link);
			}
		}
		names = {"path",          "item_id",   "permission_id", "roles",    "granted_by",     "grantee_type", "grantee",
		         "grantee_email", "link_type", "link_scope",    "link_url", "inherited_from", "expires"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP};
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

} // namespace

void RegisterPermissionFunctions(ExtensionLoader &loader) {
	TableFunction fn("sharepoint_permissions", {LogicalType::VARCHAR}, RowsScan, PermissionsBind, CursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "Who can reach a file or folder (or the files a glob matches): one row per grantee of each permission -- "
	    "roles, granted_by (direct, inherited, link), the user, group or app, and for a sharing link its type, "
	    "scope (anonymous, organization, users) and address.";
	desc.examples = {"SELECT path, grantee, roles, link_scope FROM sharepoint_permissions('sharepoint://Finance/**')"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
