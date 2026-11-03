// sharepoint_versions(path): a SharePoint file's version history (#66).
// Reading an earlier version is the filesystem's "?version=" path form
// (sharepoint_uri.hpp, sharepoint_filesystem.cpp); this lists what exists.
#include "sharepoint_versions.hpp"
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

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

//! A nextLink chain longer than this is a server bug, not a history.
constexpr int kMaxVersionPages = 1000;

struct VersionsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
}

const picojson::object *Obj(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::object>()) ? &it->second.get<picojson::object>() : nullptr;
}

unique_ptr<FunctionData> VersionsBind(ClientContext &context, TableFunctionBindInput &input,
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
		auto uri = parsed.uri;
		if (!uri.version.empty()) {
			throw InvalidInputException("sharepoint: name the file, not one of its versions: '%s'", path);
		}
		auto opener = ClientData::Get(context).file_opener.get();
		const auto auth = GetAuthContext(opener, path);
		GraphClient client(auth);
		const auto meta = uri.kind == SharepointUriKind::FILE_ID ? client.StatById(uri.item_id)
		                                                         : client.StatByPath(uri.RootRelativePath());
		if (!meta.found || meta.is_folder) {
			throw InvalidInputException("sharepoint: no such file: '%s'", path);
		}
		const string what = "the versions of '" + path + "'";
		auto data = make_uniq<VersionsBindData>();
		string next = "/v1.0/drives/" + auth.drive_id + "/items/" + meta.id + "/versions";
		for (int pages = 0; !next.empty(); pages++) {
			if (pages >= kMaxVersionPages) {
				throw IOException("sharepoint: reading %s exceeded %d pages", what, kMaxVersionPages);
			}
			picojson::value holder;
			if (!picojson::parse(holder, client.GraphGet(next, what)).empty() || !holder.is<picojson::object>()) {
				throw IOException("sharepoint: unexpected response reading %s", what);
			}
			const auto &page = holder.get<picojson::object>();
			auto values = page.find("value");
			if (values != page.end() && values->second.is<picojson::array>()) {
				for (auto &v : values->second.get<picojson::array>()) {
					if (!v.is<picojson::object>()) {
						continue;
					}
					const auto &o = v.get<picojson::object>();
					const auto id = Str(o, "id");
					// Graph lists the current version first.
					const bool current = data->rows.empty();
					auto version_uri = uri;
					version_uri.version = current ? string() : id;
					int64_t micros;
					const auto modified = ParseIsoTimestamp(Str(o, "lastModifiedDateTime"), micros)
					                          ? Value::TIMESTAMP(timestamp_t(micros))
					                          : Value(LogicalType::TIMESTAMP);
					auto size = o.find("size");
					Value modified_by(LogicalType::VARCHAR), modified_by_email(LogicalType::VARCHAR);
					if (auto by = Obj(o, "lastModifiedBy")) {
						auto who = Obj(*by, "user");
						who = who ? who : Obj(*by, "application");
						if (who && !Str(*who, "displayName").empty()) {
							modified_by = Value(Str(*who, "displayName"));
						}
						if (who && !Str(*who, "email").empty()) {
							modified_by_email = Value(Str(*who, "email"));
						}
					}
					data->rows.push_back({Value(id), Value(version_uri.ToString()), Value::BOOLEAN(current), modified,
					                      modified_by, modified_by_email,
					                      size != o.end() && size->second.is<int64_t>()
					                          ? Value::BIGINT(size->second.get<int64_t>())
					                          : Value(LogicalType::BIGINT)});
				}
			}
			auto link = page.find("@odata.nextLink");
			next = (link != page.end() && link->second.is<string>()) ? GraphPathOfLink(link->second.get<string>())
			                                                         : string();
		}
		names = {"version", "path", "current", "modified", "modified_by", "modified_by_email", "size"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::BOOLEAN, LogicalType::TIMESTAMP,
		                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT};
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

void VersionsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<VersionsBindData>();
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

void RegisterVersionFunctions(ExtensionLoader &loader) {
	TableFunction fn("sharepoint_versions", {LogicalType::VARCHAR}, VersionsScan, VersionsBind, CursorInit);
	duckdb_compat::NameParameters(fn, {"path"});
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description =
	    "A SharePoint file's version history, newest first: the version label, a path that reads that version "
	    "(sharepoint://...?version=<label>), whether it is the current one, when and by whom it was saved, and its "
	    "size.";
	desc.examples = {"SELECT * FROM sharepoint_versions('sharepoint://Finance/plan.xlsx')",
	                 "SELECT * FROM read_parquet('sharepoint://Finance/actuals.parquet?version=3.0')"};
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace sharepoint
} // namespace duckdb
