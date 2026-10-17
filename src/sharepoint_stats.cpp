// sharepoint_stats() / sharepoint_reset_stats(): the SQL face of the pure
// counter registry (sharepoint_counters.*), shaped like the sibling's
// gdrive_stats() / gdrive_reset_stats().
#include "sharepoint_stats.hpp"
#include "sharepoint_counters.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>
#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

struct StatsBindData : public TableFunctionData {
	vector<std::pair<string, int64_t>> rows;
};

unique_ptr<FunctionData> StatsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                   duckdb_compat::BindNames &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
	names = {"metric", "value"};
	auto data = make_uniq<StatsBindData>();
	// Snapshot at bind time: one consistent set of numbers per query.
	for (const auto &kv : SnapshotCounters()) {
		data->rows.emplace_back(kv.first, static_cast<int64_t>(kv.second));
	}
	return std::move(data);
}

struct CursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> CursorInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CursorState>();
}

void StatsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &rows = input.bind_data->Cast<StatsBindData>().rows;
	auto &state = input.global_state->Cast<CursorState>();
	idx_t out = 0;
	while (state.cursor < rows.size() && out < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, out, Value(rows[state.cursor].first));
		output.SetValue(1, out, Value::BIGINT(rows[state.cursor].second));
		state.cursor++;
		out++;
	}
	output.SetCardinality(out);
}

unique_ptr<FunctionData> ResetBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                   duckdb_compat::BindNames &names) {
	return_types = {LogicalType::BOOLEAN};
	names = {"success"};
	return make_uniq<TableFunctionData>();
}

void ResetScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<CursorState>();
	if (state.cursor > 0) {
		output.SetCardinality(0);
		return;
	}
	ResetCounters();
	state.cursor = 1;
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(true));
}

} // namespace

void RegisterSharepointStats(ExtensionLoader &loader) {
	{
		TableFunction fn("sharepoint_stats", {}, StatsScan, StatsBind, CursorInit);
		CreateTableFunctionInfo info(fn);
		FunctionDescription desc;
		desc.description =
		    "Microsoft Graph call counters, one row per metric: token_request, stat, read_range, list_page, "
		    "upload_put, upload_session, upload_chunk, upload_commit, upload_abort, delete, move, create_folder "
		    "(calls by kind), retries, throttled, bytes_read, bytes_written, meta_cache_hits (opens served from the "
		    "sharepoint_immutable_prefixes metadata cache), auth_refresh (401s answered by a fresh token) and "
		    "upload_buffer_peak (a HIGH-WATER MARK: the most bytes one file being written held in memory). "
		    "Process-wide; reset with sharepoint_reset_stats() "
		    "to measure what one query costs.";
		desc.examples = {"SELECT * FROM sharepoint_stats()"};
		desc.categories = {"sharepoint"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
	{
		TableFunction fn("sharepoint_reset_stats", {}, ResetScan, ResetBind, CursorInit);
		CreateTableFunctionInfo info(fn);
		FunctionDescription desc;
		desc.description = "Zero the counters reported by sharepoint_stats(). Process-wide: it disturbs a "
		                   "concurrent measurement in another connection.";
		desc.examples = {"CALL sharepoint_reset_stats()"};
		desc.categories = {"sharepoint"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace sharepoint
} // namespace duckdb
