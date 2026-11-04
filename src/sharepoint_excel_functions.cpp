// read_excel_table() / read_excel_range(): an .xlsx in SharePoint / OneDrive
// read live through the Graph workbook API -- the values Excel for the web
// holds now, by table name or address, without downloading the file. Table
// functions next to the sharepoint:// filesystem, over the same secret; they
// change nothing about it. The cell logic is pure (sharepoint_excel.*); the
// Graph shapes are recorded in docs/hld.md §9.
#include "sharepoint_excel_functions.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_copy_rewrite.hpp"
#include "sharepoint_excel.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <picojson.h>

#include <cmath>
#include <cstdint>
#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

//! Cells per range request. Graph answers a range with three parallel arrays
//! (values, valueTypes, numberFormat) and rejects or times out on very large
//! ones; 20k cells is a few MB of JSON. A 10,000 x 5 table is three requests.
constexpr int64_t kCellsPerRequest = 20000;

struct ExcelBindData : public TableFunctionData {
	vector<ExcelColumnType> types;
	//! Row-major. Read whole at bind: the column types depend on every row.
	vector<vector<ExcelCell>> rows;
	//! The workbook counts dates from 1904 (Workbook::date1904).
	bool date1904 = false;
};

//! The workbook an argument names, and what is needed to call Graph for it.
struct Workbook {
	AuthContext auth;
	string item_id;
	string path; // as the user can paste it back, for messages
	//! Dates count from 1904 (old Mac Excel), not 1900 (#64).
	bool date1904 = false;
};

//! Whether the workbook uses the 1904 date system. The workbook API does not
//! say; xl/workbook.xml does. Three ranged reads: the zip's tail (end record),
//! its central directory, then that one entry. Any surprise means 1900, the
//! default: this must never fail a read.
bool UsesDate1904(GraphClient &client, const string &item_id, int64_t size) {
	try {
		constexpr int64_t kTail = 64 * 1024 + 22; // end record + longest comment
		constexpr uint64_t kMaxEntry = 8 * 1024 * 1024;
		if (size < 22) {
			return false;
		}
		const int64_t tail_at = std::max<int64_t>(0, size - kTail);
		string tail;
		client.ReadRange(item_id, tail_at, size - tail_at, tail);
		uint64_t cd_offset, cd_size;
		if (!ZipCentralDirectory(tail, static_cast<uint64_t>(size), cd_offset, cd_size) || cd_size > kMaxEntry) {
			return false;
		}
		string cd;
		if (cd_offset >= static_cast<uint64_t>(tail_at)) {
			cd = tail.substr(cd_offset - tail_at, cd_size);
		} else {
			client.ReadRange(item_id, static_cast<int64_t>(cd_offset), static_cast<int64_t>(cd_size), cd);
		}
		ZipEntry entry;
		if (!ZipFindEntry(cd, "xl/workbook.xml", entry) || entry.compressed_size > kMaxEntry ||
		    (entry.method != 0 && entry.method != 8)) {
			return false;
		}
		// The local header's name and extra field may differ from the central
		// record's; read generously and measure.
		string local;
		client.ReadRange(item_id, static_cast<int64_t>(entry.local_header_offset),
		                 static_cast<int64_t>(30 + 1024 + entry.compressed_size), local);
		uint64_t header;
		if (!ZipLocalHeaderSize(local, header) || header + entry.compressed_size > local.size()) {
			return false;
		}
		auto data = local.substr(header, entry.compressed_size);
		if (entry.method == 8) {
			data = GZipFileSystem::UncompressGZIPString(
			    GzipFrame(data, entry.crc32, static_cast<uint32_t>(entry.uncompressed_size)));
		}
		return WorkbookUses1904(data);
	} catch (std::exception &) {
		return false;
	}
}

Workbook OpenWorkbook(ClientContext &context, const string &arg) {
	Workbook wb;
	// The scheme is optional: read_excel_table('Finance/plan.xlsx', ...) means
	// the secret's drive, as sharepoint://Finance/plan.xlsx would.
	wb.path = IsSharepointUri(arg) ? arg : string(SHAREPOINT_SCHEME) + arg;
	auto parsed = ParseSharepointUri(wb.path);
	if (!parsed.ok) {
		throw InvalidInputException("sharepoint: %s", parsed.error);
	}
	const auto &uri = parsed.uri;
	if (uri.kind == SharepointUriKind::PATH && uri.segments.empty()) {
		throw InvalidInputException("sharepoint: '%s' is the drive root, not a workbook", wb.path);
	}
	auto opener = ClientData::Get(context).file_opener.get();
	wb.auth = GetAuthContext(opener, wb.path);
	GraphClient client(wb.auth);
	const auto meta =
	    uri.kind == SharepointUriKind::PATH ? client.StatByPath(uri.RootRelativePath()) : client.StatById(uri.item_id);
	if (!meta.found) {
		throw InvalidInputException("sharepoint: workbook '%s' not found", wb.path);
	}
	if (meta.is_folder) {
		throw InvalidInputException("sharepoint: '%s' is a folder, not a workbook", wb.path);
	}
	wb.item_id = meta.id;
	wb.date1904 = UsesDate1904(client, meta.id, meta.size);
	return wb;
}

const picojson::array *ArrayField(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return (it != o.end() && it->second.is<picojson::array>()) ? &it->second.get<picojson::array>() : nullptr;
}

const picojson::value *At(const picojson::array *rows, size_t r, size_t c) {
	if (!rows || r >= rows->size() || !(*rows)[r].is<picojson::array>()) {
		return nullptr;
	}
	auto &row = (*rows)[r].get<picojson::array>();
	return c < row.size() ? &row[c] : nullptr;
}

bool AsNumber(const picojson::value &v, double &out) {
	if (v.is<int64_t>()) {
		out = static_cast<double>(v.get<int64_t>());
		return true;
	}
	if (v.is<double>()) {
		out = v.get<double>();
		return true;
	}
	return false;
}

ExcelCell ToCell(const picojson::value *value, const picojson::value *type, const picojson::value *format) {
	ExcelCell cell;
	if (format && format->is<string>()) {
		cell.format = format->get<string>();
	}
	if (!value || value->is<picojson::null>()) {
		return cell;
	}
	const string kind = (type && type->is<string>()) ? type->get<string>() : string();
	if (kind == "Empty") {
		return cell;
	}
	if (kind == "Error") {
		cell.kind = ExcelCellKind::ERROR;
		cell.text = value->is<string>() ? value->get<string>() : value->to_str();
		return cell;
	}
	// String, Integer, Double, Boolean -- and Unknown, read by the JSON type.
	if (value->is<bool>()) {
		cell.kind = ExcelCellKind::BOOLEAN;
		cell.boolean = value->get<bool>();
	} else if (kind != "String" && AsNumber(*value, cell.number)) {
		cell.kind = ExcelCellKind::NUMBER;
	} else if (value->is<string>()) {
		if (!value->get<string>().empty()) {
			cell.kind = ExcelCellKind::STRING;
			cell.text = value->get<string>();
		}
	} else {
		cell.kind = ExcelCellKind::STRING;
		cell.text = value->to_str();
	}
	return cell;
}

picojson::object ParseObject(const string &body, const string &what) {
	picojson::value v;
	if (!picojson::parse(v, body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected workbook response reading %s", what);
	}
	return v.get<picojson::object>();
}

ExcelAddress ParseGraphAddress(const picojson::object &o, const string &what) {
	auto it = o.find("address");
	if (it == o.end() || !it->second.is<string>()) {
		throw IOException("sharepoint: workbook response for %s has no address", what);
	}
	auto parsed = ParseExcelAddress(it->second.get<string>());
	if (!parsed.ok || !parsed.address.HasCells()) {
		throw IOException("sharepoint: unexpected address '%s' for %s", it->second.get<string>(), what);
	}
	return parsed.address;
}

string Quoted(const string &kind, const string &name, const Workbook &wb) {
	return kind + " '" + name + "' in '" + wb.path + "'";
}

//! Every cell of `address`, row-major, read a window of rows at a time.
vector<vector<ExcelCell>> ReadCells(GraphClient &client, const Workbook &wb, const ExcelAddress &address) {
	vector<vector<ExcelCell>> rows;
	const auto cols = static_cast<size_t>(address.ColCount());
	for (auto &w : SplitExcelRows(address, kCellsPerRequest)) {
		const string cells = ExcelColumnLetters(w.first_col) + std::to_string(w.first_row) + ":" +
		                     ExcelColumnLetters(w.last_col) + std::to_string(w.last_row);
		const string what = Quoted("worksheet", w.sheet, wb);
		const auto body =
		    client.WorkbookGet(wb.item_id,
		                       "worksheets/" + EncodeWorkbookName(w.sheet) + "/range(address='" +
		                           EncodeWorkbookName(cells) + "')?$select=values,valueTypes,numberFormat",
		                       what);
		const auto o = ParseObject(body, what);
		auto values = ArrayField(o, "values");
		auto types = ArrayField(o, "valueTypes");
		auto formats = ArrayField(o, "numberFormat");
		if (!values) {
			throw IOException("sharepoint: workbook response for %s has no values", what);
		}
		for (size_t r = 0; r < static_cast<size_t>(w.RowCount()); r++) {
			vector<ExcelCell> row;
			row.reserve(cols);
			for (size_t c = 0; c < cols; c++) {
				row.push_back(ToCell(At(values, r, c), At(types, r, c), At(formats, r, c)));
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

LogicalType ToLogicalType(ExcelColumnType t) {
	switch (t) {
	case ExcelColumnType::DOUBLE:
		return LogicalType::DOUBLE;
	case ExcelColumnType::BIGINT:
		return LogicalType::BIGINT;
	case ExcelColumnType::BOOLEAN:
		return LogicalType::BOOLEAN;
	case ExcelColumnType::DATE:
		return LogicalType::DATE;
	case ExcelColumnType::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case ExcelColumnType::TIME:
		return LogicalType::TIME;
	default:
		return LogicalType::VARCHAR;
	}
}

Value ToValue(const ExcelCell &cell, ExcelColumnType t, bool date1904) {
	const auto type = ToLogicalType(t);
	if (cell.kind == ExcelCellKind::EMPTY) {
		return Value(type);
	}
	if (t == ExcelColumnType::VARCHAR) {
		return Value(ExcelCellText(cell));
	}
	if (t == ExcelColumnType::BOOLEAN) {
		return cell.kind == ExcelCellKind::BOOLEAN ? Value::BOOLEAN(cell.boolean) : Value(type);
	}
	if (cell.kind != ExcelCellKind::NUMBER) {
		return Value(type); // an error cell (#DIV/0!) in a typed column
	}
	int64_t days, micros;
	// A 1904 workbook's serial is the 1900 serial less 1462 days. Times of
	// day (TIME) are the same in both.
	const double serial = cell.number + (date1904 ? kExcel1904Offset : 0);
	switch (t) {
	case ExcelColumnType::BIGINT:
		return Value::BIGINT(static_cast<int64_t>(cell.number));
	case ExcelColumnType::DOUBLE:
		return Value::DOUBLE(cell.number);
	case ExcelColumnType::DATE:
		ExcelSerialToEpoch(serial, days, micros);
		return Value::DATE(date_t(static_cast<int32_t>(days)));
	case ExcelColumnType::TIMESTAMP:
		ExcelSerialToEpoch(serial, days, micros);
		return Value::TIMESTAMP(timestamp_t(days * Interval::MICROS_PER_DAY + micros));
	case ExcelColumnType::TIME:
		ExcelSerialToEpoch(cell.number, days, micros);
		return Value::TIME(dtime_t(micros));
	default:
		return Value(type);
	}
}

bool BoolOption(TableFunctionBindInput &input, const char *name, bool fallback) {
	auto value = duckdb_compat::FindNamedParameter(input.named_parameters, name);
	return (value && !value->IsNull()) ? BooleanValue::Get(*value) : fallback;
}

string StringArg(TableFunctionBindInput &input, idx_t i, const char *what) {
	if (input.inputs[i].IsNull()) {
		throw InvalidInputException("sharepoint: the %s must not be NULL", what);
	}
	return StringValue::Get(input.inputs[i]);
}

//! Shared tail of both binds: types per column, then the result schema.
unique_ptr<FunctionData> Finish(vector<string> names, vector<vector<ExcelCell>> rows, bool all_varchar, bool date1904,
                                vector<LogicalType> &return_types, duckdb_compat::BindNames &out_names) {
	auto data = make_uniq<ExcelBindData>();
	for (size_t c = 0; c < names.size(); c++) {
		vector<const ExcelCell *> column;
		column.reserve(rows.size());
		for (auto &row : rows) {
			column.push_back(&row[c]);
		}
		const auto t = all_varchar ? ExcelColumnType::VARCHAR : InferExcelColumnType(column);
		data->types.push_back(t);
		return_types.push_back(ToLogicalType(t));
		out_names.emplace_back(names[c]);
	}
	data->rows = std::move(rows);
	data->date1904 = date1904;
	return std::move(data);
}

//! read_excel_table's bind, for the table function and an ATTACHed site's
//! workbook tables (#100).
unique_ptr<FunctionData> BindTable(ClientContext &context, const string &path, const string &table, bool all_varchar,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	{
		auto wb = OpenWorkbook(context, path);
		GraphClient client(wb.auth);
		const string what = Quoted("table", table, wb);
		const string base = "tables/" + EncodeWorkbookName(table);

		// The header row holds the column names (unique in an Excel table);
		// the data body's address says where the rows are.
		const auto header =
		    ParseObject(client.WorkbookGet(wb.item_id, base + "/headerRowRange?$select=values", what), what);
		auto header_values = ArrayField(header, "values");
		const auto body =
		    ParseObject(client.WorkbookGet(wb.item_id, base + "/dataBodyRange?$select=address", what), what);
		const auto address = ParseGraphAddress(body, what);

		vector<ExcelCell> header_cells;
		for (size_t c = 0; c < static_cast<size_t>(address.ColCount()); c++) {
			header_cells.push_back(ToCell(At(header_values, 0, c), nullptr, nullptr));
		}
		result =
		    Finish(ExcelHeaderNames(header_cells), ReadCells(client, wb, address), all_varchar, wb.date1904, return_types,
		           names);
	}
	return result;
}

unique_ptr<FunctionData> TableBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		result = BindTable(context, StringArg(input, 0, "workbook path"), StringArg(input, 1, "table name"),
		                   BoolOption(input, "all_varchar", false), return_types, names);
	});
	return result;
}

unique_ptr<FunctionData> RangeBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto range = StringArg(input, 1, "range address");
		const bool header = BoolOption(input, "header", true);
		const bool all_varchar = BoolOption(input, "all_varchar", false);
		auto parsed = ParseExcelAddress(range);
		if (!parsed.ok) {
			throw InvalidInputException("sharepoint: %s", parsed.error);
		}
		auto wb = OpenWorkbook(context, StringArg(input, 0, "workbook path"));
		GraphClient client(wb.auth);
		auto address = parsed.address;
		if (!address.HasCells()) {
			// A bare sheet: its used range. valuesOnly ignores cells that are
			// merely formatted.
			const string what = Quoted("worksheet", address.sheet, wb);
			const auto used = ParseObject(client.WorkbookGet(wb.item_id,
			                                                 "worksheets/" + EncodeWorkbookName(address.sheet) +
			                                                     "/usedRange(valuesOnly=true)?$select=address",
			                                                 what),
			                              what);
			address = ParseGraphAddress(used, what);
		}
		auto rows = ReadCells(client, wb, address);
		vector<ExcelCell> header_cells(static_cast<size_t>(address.ColCount()));
		if (header && !rows.empty()) {
			header_cells = std::move(rows.front());
			rows.erase(rows.begin());
		}
		result = Finish(ExcelHeaderNames(header_cells), std::move(rows), all_varchar, wb.date1904, return_types, names);
	});
	return result;
}

struct CursorState : public GlobalTableFunctionState {
	idx_t cursor = 0;
};

unique_ptr<GlobalTableFunctionState> CursorInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CursorState>();
}

void ExcelScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ExcelBindData>();
	auto &state = input.global_state->Cast<CursorState>();
	idx_t out = 0;
	while (state.cursor < data.rows.size() && out < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.cursor];
		for (idx_t c = 0; c < data.types.size(); c++) {
			output.SetValue(c, out, ToValue(row[c], data.types[c], data.date1904));
		}
		state.cursor++;
		out++;
	}
	duckdb_compat::SetChildCardinality(output, out);
}

void Register(ExtensionLoader &loader, TableFunction fn, const vector<string> &parameters, const string &description,
              const vector<string> &examples) {
	duckdb_compat::NameParameters(fn, parameters);
	duckdb_compat::AddNamedParameter(fn, "all_varchar", LogicalType::BOOLEAN);
	CreateTableFunctionInfo info(fn);
	FunctionDescription desc;
	desc.description = description;
	desc.examples = examples;
	desc.categories = {"sharepoint", "excel"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

// ===========================================================================
// COPY ... TO '<workbook>' (FORMAT excel_table, TABLE 'Forecast')
// COPY ... TO '<workbook>' (FORMAT excel_range, RANGE 'Forecast!A1')
//
// Writes go through the workbook API (rows/add, a range PATCH), never by
// replacing the file -- people can keep the workbook open in Excel while SQL
// fills it.
//
// The public formats only rewrite the statement (CopyFunction::plan, as
// DuckDB's own JSON writer does) to an internal format with USE_TMP_FILE
// false. Without that, COPY to an EXISTING target writes "tmp_<name>" and
// renames it over the target at the end -- here a MoveFile of the workbook
// itself. See docs/hld.md §9.
// ===========================================================================

constexpr const char *kTableFormat = "excel_table";
constexpr const char *kRangeFormat = "excel_range";
constexpr const char *kTableFormatInternal = "__sharepoint_excel_table";
constexpr const char *kRangeFormatInternal = "__sharepoint_excel_range";

//! How one column's values are written.
enum class ExcelWriteKind { TEXT, NUMBER, BOOLEAN, DATE, TIMESTAMP, TIME };

ExcelWriteKind WriteKindFor(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return ExcelWriteKind::BOOLEAN;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return ExcelWriteKind::NUMBER;
	case LogicalTypeId::DATE:
		return ExcelWriteKind::DATE;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
		return ExcelWriteKind::TIMESTAMP;
	case LogicalTypeId::TIME:
		return ExcelWriteKind::TIME;
	default:
		return ExcelWriteKind::TEXT; // VARCHAR, and anything else as its text
	}
}

//! The number format a column's cells get, or nullptr to leave it alone.
const char *NumberFormatFor(ExcelWriteKind kind) {
	switch (kind) {
	case ExcelWriteKind::DATE:
		return "yyyy-mm-dd";
	case ExcelWriteKind::TIMESTAMP:
		return "yyyy-mm-dd hh:mm:ss";
	case ExcelWriteKind::TIME:
		return "hh:mm:ss";
	default:
		return nullptr;
	}
}

//! A value as the workbook API takes it. NULL and non-finite numbers clear
//! the cell (""); text keeps its apostrophe guard (ExcelTextInput).
picojson::value ToJsonCell(const Value &v, ExcelWriteKind kind, bool date1904) {
	const double epoch_shift = date1904 ? kExcel1904Offset : 0;
	if (v.IsNull()) {
		return picojson::value(string());
	}
	switch (kind) {
	case ExcelWriteKind::BOOLEAN:
		return picojson::value(BooleanValue::Get(v));
	case ExcelWriteKind::NUMBER: {
		const double d = v.GetValue<double>();
		return std::isfinite(d) ? picojson::value(d) : picojson::value(string());
	}
	case ExcelWriteKind::DATE: {
		const auto date = v.GetValue<date_t>();
		if (!Value::IsFinite(date)) {
			return picojson::value(string());
		}
		return picojson::value(ExcelSerialFromEpoch(date.days, 0) - epoch_shift);
	}
	case ExcelWriteKind::TIMESTAMP: {
		const auto ts = v.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		if (!Value::IsFinite(ts)) {
			return picojson::value(string());
		}
		const int64_t micros = Timestamp::GetEpochMicroSeconds(ts);
		int64_t days = micros / Interval::MICROS_PER_DAY;
		int64_t rest = micros % Interval::MICROS_PER_DAY;
		if (rest < 0) {
			rest += Interval::MICROS_PER_DAY;
			days--;
		}
		return picojson::value(ExcelSerialFromEpoch(days, rest) - epoch_shift);
	}
	case ExcelWriteKind::TIME: {
		// Time::Convert, not dtime_t's member: `micros` before 2.0, `value` in 2.0.
		int32_t hour, minute, second, micros;
		Time::Convert(v.GetValue<dtime_t>(), hour, minute, second, micros);
		const int64_t of_day = ((hour * 60LL + minute) * 60 + second) * 1000000 + micros;
		return picojson::value(ExcelSerialFromEpoch(-25569, of_day));
	}
	default:
		return picojson::value(ExcelTextInput(v.ToString()));
	}
}

string CopyStringOption(const CopyInfo &info, const char *name) {
	auto v = FindCopyOption(info.options, name);
	if (!v || v->size() != 1 || (*v)[0].IsNull()) {
		return string();
	}
	return (*v)[0].ToString();
}

bool CopyBoolOption(ClientContext &context, const CopyInfo &info, const char *name, bool fallback) {
	auto v = FindCopyOption(info.options, name);
	if (!v) {
		return fallback;
	}
	// A bare `HEADER` means true, as in COPY's own options.
	return v->empty() || (*v)[0].DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
}

struct ExcelWriteBindData : public FunctionData {
	bool table_mode = false;
	string table;        // excel_table: the table to append to (or create)
	string sheet;        // excel_table: where to create a missing table
	ExcelAddress anchor; // excel_range: the top-left cell
	bool header = true;  // excel_range: write the column names first
	//! OVERWRITE: excel_table replaces the table's rows; excel_range first
	//! clears from the anchor down and right (ExcelClearRegion).
	bool overwrite = false;
	vector<string> names;
	vector<ExcelWriteKind> kinds;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ExcelWriteBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ExcelWriteBindData>();
		return table_mode == other.table_mode && table == other.table && names == other.names;
	}
};

unique_ptr<FunctionData> ExcelWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                        const duckdb_compat::CopyBindNames &names, const vector<LogicalType> &sql_types,
                                        bool table_mode) {
	auto &info = input.info;
	// Only reachable through the rewrite, which pins USE_TMP_FILE false and
	// leaves this marker (the binder consumes USE_TMP_FILE itself before bind).
	// A direct FORMAT __sharepoint_excel_* could otherwise rename over the file.
	if (!IsRewrittenInPlace(info)) {
		throw InvalidInputException("sharepoint: write workbooks with FORMAT %s",
		                            table_mode ? kTableFormat : kRangeFormat);
	}
	auto data = make_uniq<ExcelWriteBindData>();
	data->table_mode = table_mode;
	if (table_mode) {
		data->table = CopyStringOption(info, "table");
		if (data->table.empty()) {
			throw InvalidInputException("sharepoint: FORMAT excel_table needs TABLE '<name>'");
		}
		data->sheet = CopyStringOption(info, "sheet");
		if (data->sheet.empty()) {
			data->sheet = data->table;
		}
	} else {
		const auto range = CopyStringOption(info, "range");
		if (range.empty()) {
			throw InvalidInputException("sharepoint: FORMAT excel_range needs RANGE 'Sheet!A1'");
		}
		auto parsed = ParseExcelAddress(range);
		if (!parsed.ok) {
			throw InvalidInputException("sharepoint: %s", parsed.error);
		}
		data->anchor = parsed.address;
		data->header = CopyBoolOption(context, info, "header", true);
	}
	data->overwrite = FindCopyOption(info.options, kOverwriteMarker) != nullptr;
	for (idx_t i = 0; i < sql_types.size(); i++) {
		data->names.push_back(duckdb_compat::Str(names[i]));
		data->kinds.push_back(WriteKindFor(sql_types[i]));
	}
	return std::move(data);
}

unique_ptr<FunctionData> ExcelTableWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                             const duckdb_compat::CopyBindNames &names,
                                             const vector<LogicalType> &sql_types) {
	return ExcelWriteBind(context, input, names, sql_types, true);
}

unique_ptr<FunctionData> ExcelRangeWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                             const duckdb_compat::CopyBindNames &names,
                                             const vector<LogicalType> &sql_types) {
	return ExcelWriteBind(context, input, names, sql_types, false);
}

struct ExcelWriteGlobalState : public GlobalFunctionData {
	Workbook wb;
	string session_id;
	//! Where the next block goes. Range mode: the next free row of the block.
	//! Table mode: the table's columns on its sheet, and the row after its body.
	ExcelAddress target;
	int64_t next_row = 0;
	//! Range writes into a fresh sheet; the table is created around them at
	//! the end (a missing excel_table target).
	bool create_table = false;
	//! Table mode: the table column each COPY column goes to.
	vector<idx_t> column_of;
	idx_t width = 0;
	bool any_number_format = false;
	picojson::array pending_values;
	picojson::array pending_formats;
	idx_t pending_cells = 0;
	idx_t first_data_row = 0; // created table: the header row
	//! A worksheet this COPY added (EnsureWorksheet), waited for at the end.
	string created_sheet;
};

string WorksheetResource(const string &sheet) {
	return "worksheets/" + EncodeWorkbookName(sheet);
}

string RangeResource(const ExcelAddress &block) {
	const string cells = ExcelColumnLetters(block.first_col) + std::to_string(block.first_row) + ":" +
	                     ExcelColumnLetters(block.last_col) + std::to_string(block.last_row);
	return WorksheetResource(block.sheet) + "/range(address='" + EncodeWorkbookName(cells) + "')";
}

//! Make sure `sheet` exists; true if it was created (so it is empty).
bool EnsureWorksheet(GraphClient &client, ExcelWriteGlobalState &g, const string &sheet) {
	int status = 0;
	const string what = Quoted("worksheet", sheet, g.wb);
	client.WorkbookCall("GET", g.wb.item_id, WorksheetResource(sheet) + "?$select=name", string(), g.session_id, what,
	                    true, &status);
	if (status != 404) {
		return false;
	}
	picojson::object body;
	body["name"] = picojson::value(sheet);
	client.WorkbookCall("POST", g.wb.item_id, "worksheets/add", picojson::value(body).serialize(), g.session_id, what,
	                    false);
	g.created_sheet = sheet;
	return true;
}

//! True if `sheet` holds no values (its used range is one empty cell).
bool WorksheetIsEmpty(GraphClient &client, ExcelWriteGlobalState &g, const string &sheet) {
	const string what = Quoted("worksheet", sheet, g.wb);
	const auto used =
	    ParseObject(client.WorkbookCall("GET", g.wb.item_id,
	                                    WorksheetResource(sheet) + "/usedRange(valuesOnly=true)?$select=address,values",
	                                    string(), g.session_id, what, true),
	                what);
	const auto address = ParseGraphAddress(used, what);
	if (address.RowCount() != 1 || address.ColCount() != 1) {
		return false;
	}
	auto v = At(ArrayField(used, "values"), 0, 0);
	return !v || (v->is<string>() && v->get<string>().empty());
}

void QueueRow(ExcelWriteGlobalState &g, picojson::array values, picojson::array formats) {
	g.pending_cells += values.size();
	g.pending_values.emplace_back(std::move(values));
	g.pending_formats.emplace_back(std::move(formats));
}

void FlushPending(ExcelWriteGlobalState &g, const ExcelWriteBindData &bind, bool table_append) {
	if (g.pending_values.empty()) {
		return;
	}
	GraphClient client(g.wb.auth);
	const auto rows = static_cast<int64_t>(g.pending_values.size());
	ExcelAddress block = g.target;
	block.first_row = g.next_row;
	block.last_row = g.next_row + rows - 1;
	if (table_append) {
		const string what = Quoted("table", bind.table, g.wb);
		picojson::object body;
		body["values"] = picojson::value(g.pending_values);
		ParseObject(client.WorkbookCall("POST", g.wb.item_id, "tables/" + EncodeWorkbookName(bind.table) + "/rows/add",
		                                picojson::value(body).serialize(), g.session_id, what, false),
		            what);
		// Dates are written as serials; give the new rows their formats.
		if (g.any_number_format) {
			picojson::object formats;
			formats["numberFormat"] = picojson::value(g.pending_formats);
			client.WorkbookCall("PATCH", g.wb.item_id, RangeResource(block), picojson::value(formats).serialize(),
			                    g.session_id, what, true);
		}
	} else {
		string error;
		if (!ExcelBlockAt(block, rows, g.target.ColCount(), block, error)) {
			throw InvalidInputException("sharepoint: %s", error);
		}
		picojson::object body;
		body["values"] = picojson::value(g.pending_values);
		if (g.any_number_format) {
			body["numberFormat"] = picojson::value(g.pending_formats);
		}
		client.WorkbookCall("PATCH", g.wb.item_id, RangeResource(block), picojson::value(body).serialize(),
		                    g.session_id, Quoted("range", FormatExcelAddress(block), g.wb), true);
	}
	g.next_row += rows;
	g.pending_values.clear();
	g.pending_formats.clear();
	g.pending_cells = 0;
}

//! Closes the COPY's workbook session if initialisation throws after opening
//! it; disarmed once initialisation succeeds (finalize closes it then).
struct SessionGuard {
	ExcelWriteGlobalState &g;
	bool armed = true;
	~SessionGuard() {
		if (!armed || g.session_id.empty()) {
			return;
		}
		try {
			GraphClient client(g.wb.auth);
			client.WorkbookCall("POST", g.wb.item_id, "closeSession", "{}", g.session_id, "workbook session", true);
		} catch (...) {
			// Best effort: an unclosed session expires on its own.
		}
		g.session_id.clear();
	}
};

unique_ptr<GlobalFunctionData> ExcelWriteInitGlobal(ClientContext &context, FunctionData &bind_p,
                                                    const string &file_path) {
	auto &bind = bind_p.Cast<ExcelWriteBindData>();
	auto g = make_uniq<ExcelWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		g->wb = OpenWorkbook(context, file_path);
		GraphClient client(g->wb.auth);
		// One persistent session for the whole COPY: fewer workbook opens, and
		// every write lands in the saved file. Retried like a read: a second
		// session after a lost reply is harmless (it expires unused), and
		// Graph does answer createSession with a transient 504 (live CI).
		const auto session =
		    ParseObject(client.WorkbookCall("POST", g->wb.item_id, "createSession", "{\"persistChanges\":true}",
		                                    string(), "workbook session for '" + g->wb.path + "'", true),
		                "workbook session");
		auto id = session.find("id");
		if (id != session.end() && id->second.is<string>()) {
			g->session_id = id->second.get<string>();
		}
		for (auto kind : bind.kinds) {
			g->any_number_format = g->any_number_format || NumberFormatFor(kind) != nullptr;
		}
		// A COPY that fails from here on must not leave its session open: an
		// open session keeps the workbook locked (HTTP 423) for minutes.
		SessionGuard guard {*g};

		ExcelAddress anchor;
		bool write_header = false;
		if (bind.table_mode) {
			const string what = Quoted("table", bind.table, g->wb);
			const string base = "tables/" + EncodeWorkbookName(bind.table);
			int status = 0;
			const auto header_body = client.WorkbookCall("GET", g->wb.item_id, base + "/headerRowRange?$select=values",
			                                             string(), g->session_id, what, true, &status);
			if (status == 404) {
				// No such table: create it on its own sheet, header first.
				if (!EnsureWorksheet(client, *g, bind.sheet) && !WorksheetIsEmpty(client, *g, bind.sheet)) {
					throw InvalidInputException("sharepoint: table '%s' does not exist, and worksheet '%s' in '%s' "
					                            "already holds data; name an empty or new sheet with SHEET '<name>'",
					                            bind.table, bind.sheet, g->wb.path);
				}
				g->create_table = true;
				anchor.sheet = bind.sheet;
				write_header = true;
			} else if (bind.overwrite) {
				// Replace: turn the table back into cells, clear them, and
				// create it anew from its old top-left cell (the create path).
				const auto range =
				    ParseObject(client.WorkbookCall("GET", g->wb.item_id, base + "/range?$select=address", string(),
				                                    g->session_id, what, true),
				                what);
				const auto old_block = ParseGraphAddress(range, what);
				client.WorkbookCall("POST", g->wb.item_id, base + "/convertToRange", "{}", g->session_id, what, false);
				client.WorkbookCall("POST", g->wb.item_id, RangeResource(old_block) + "/clear", "{\"applyTo\":\"All\"}",
				                    g->session_id, what, true);
				g->create_table = true;
				anchor = old_block;
				write_header = true;
			} else {
				// Append: map COPY columns onto the table's columns by name.
				auto header = ParseObject(header_body, what);
				auto values = ArrayField(header, "values");
				const auto body =
				    ParseObject(client.WorkbookCall("GET", g->wb.item_id, base + "/dataBodyRange?$select=address",
				                                    string(), g->session_id, what, true),
				                what);
				g->target = ParseGraphAddress(body, what);
				g->next_row = g->target.last_row + 1;
				g->width = static_cast<idx_t>(g->target.ColCount());
				vector<string> table_names;
				for (idx_t c = 0; c < g->width; c++) {
					table_names.push_back(ExcelCellText(ToCell(At(values, 0, c), nullptr, nullptr)));
				}
				for (auto &name : bind.names) {
					idx_t found = DConstants::INVALID_INDEX;
					for (idx_t c = 0; c < table_names.size(); c++) {
						if (StringUtil::CIEquals(table_names[c], name)) {
							found = c;
						}
					}
					if (found == DConstants::INVALID_INDEX) {
						throw InvalidInputException("sharepoint: table '%s' in '%s' has no column \"%s\" (it has: %s)",
						                            bind.table, g->wb.path, name, StringUtil::Join(table_names, ", "));
					}
					g->column_of.push_back(found);
				}
				guard.armed = false;
				return std::move(g);
			}
		} else {
			anchor = bind.anchor;
			const bool created = EnsureWorksheet(client, *g, anchor.sheet);
			write_header = bind.header;
			if (bind.overwrite && !created) {
				const string what = Quoted("worksheet", anchor.sheet, g->wb);
				const auto used = ParseObject(
				    client.WorkbookCall("GET", g->wb.item_id,
				                        WorksheetResource(anchor.sheet) + "/usedRange(valuesOnly=true)?$select=address",
				                        string(), g->session_id, what, true),
				    what);
				ExcelAddress region;
				if (ExcelClearRegion(anchor, ParseGraphAddress(used, what), region)) {
					client.WorkbookCall("POST", g->wb.item_id, RangeResource(region) + "/clear",
					                    "{\"applyTo\":\"Contents\"}", g->session_id, what, true);
				}
			}
		}
		// Range writing (excel_range, or a table being created).
		g->width = bind.names.size();
		string error;
		if (!ExcelBlockAt(anchor, 1, static_cast<int64_t>(g->width), g->target, error)) {
			throw InvalidInputException("sharepoint: %s", error);
		}
		g->next_row = g->target.first_row;
		g->first_data_row = static_cast<idx_t>(g->target.first_row);
		if (write_header) {
			picojson::array values, formats;
			for (auto &name : bind.names) {
				values.emplace_back(ExcelTextInput(name));
				formats.emplace_back(); // null: leave the format alone
			}
			QueueRow(*g, std::move(values), std::move(formats));
		}
		guard.armed = false;
	});
	return std::move(g);
}

unique_ptr<LocalFunctionData> ExcelWriteInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

void ExcelWriteSink(ExecutionContext &context, FunctionData &bind_p, GlobalFunctionData &gstate_p, LocalFunctionData &,
                    DataChunk &input) {
	auto &bind = bind_p.Cast<ExcelWriteBindData>();
	auto &g = gstate_p.Cast<ExcelWriteGlobalState>();
	const bool append = bind.table_mode && !g.create_table;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		for (idx_t r = 0; r < input.size(); r++) {
			picojson::array values(g.width, picojson::value(string()));
			picojson::array formats(g.width);
			for (idx_t c = 0; c < input.ColumnCount(); c++) {
				const idx_t to = append ? g.column_of[c] : c;
				values[to] = ToJsonCell(input.GetValue(c, r), bind.kinds[c], g.wb.date1904);
				if (auto format = NumberFormatFor(bind.kinds[c])) {
					formats[to] = picojson::value(string(format));
				}
			}
			QueueRow(g, std::move(values), std::move(formats));
			if (g.pending_cells >= static_cast<idx_t>(kCellsPerRequest)) {
				FlushPending(g, bind, append);
			}
		}
	});
}

void ExcelWriteCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

//! Polls for a new table or worksheet to become visible outside its session
//! (~30 s of backoff in all).
constexpr int kMaxCreatedVisibleAttempts = 6;

void ExcelWriteFinalize(ClientContext &context, FunctionData &bind_p, GlobalFunctionData &gstate_p) {
	auto &bind = bind_p.Cast<ExcelWriteBindData>();
	auto &g = gstate_p.Cast<ExcelWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		FlushPending(g, bind, bind.table_mode && !g.create_table);
		GraphClient client(g.wb.auth);
		if (g.create_table) {
			// The header and rows are on the sheet; make them the table.
			ExcelAddress block = g.target;
			block.first_row = static_cast<int64_t>(g.first_data_row);
			block.last_row = std::max<int64_t>(g.next_row - 1, block.first_row);
			const string what = Quoted("table", bind.table, g.wb);
			picojson::object add;
			add["address"] = picojson::value(FormatExcelAddress(block));
			add["hasHeaders"] = picojson::value(true);
			const auto created =
			    ParseObject(client.WorkbookCall("POST", g.wb.item_id, "tables/add", picojson::value(add).serialize(),
			                                    g.session_id, what, false),
			                what);
			auto id = created.find("id");
			if (id == created.end() || !id->second.is<string>()) {
				throw IOException("sharepoint: creating %s returned no table id", what);
			}
			picojson::object rename;
			rename["name"] = picojson::value(bind.table);
			client.WorkbookCall("PATCH", g.wb.item_id, "tables/" + EncodeWorkbookName(id->second.get<string>()),
			                    picojson::value(rename).serialize(), g.session_id, what, true);
		}
		if (!g.session_id.empty()) {
			client.WorkbookCall("POST", g.wb.item_id, "closeSession", "{}", g.session_id,
			                    "workbook session for '" + g.wb.path + "'", true);
			g.session_id.clear();
		}
		// What a COPY creates in a session -- a table, a worksheet -- is not
		// at once visible outside it: a read right after the COPY got 404
		// ItemNotFound (live CI, 2026-11-03, both). Wait until it is, so
		// COPY-then-read works; the rows are written either way, so running
		// out of patience is no error.
		string created, what;
		if (g.create_table) {
			created = "tables/" + EncodeWorkbookName(bind.table);
			what = Quoted("table", bind.table, g.wb);
		} else if (!g.created_sheet.empty()) {
			created = WorksheetResource(g.created_sheet);
			what = Quoted("worksheet", g.created_sheet, g.wb);
		}
		for (int attempt = 1; !created.empty() && attempt <= kMaxCreatedVisibleAttempts; attempt++) {
			int status = 0;
			client.WorkbookCall("GET", g.wb.item_id, created + "?$select=name", string(), string(), what, true,
			                    &status);
			if (status != 404) {
				break;
			}
			SleepBeforeRetry(attempt, 0);
		}
	});
}

CopyFunctionExecutionMode ExcelWriteExecutionMode(bool, bool) {
	// One writer, rows in query order: a workbook range has no "later".
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

void ExcelTableCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, kInPlaceMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, kOverwriteMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, "table", LogicalType::VARCHAR);
	AddCopyOption(input.options, "sheet", LogicalType::VARCHAR);
}

void ExcelRangeCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, kInPlaceMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, kOverwriteMarker, LogicalType::BOOLEAN);
	AddCopyOption(input.options, "range", LogicalType::VARCHAR);
	AddCopyOption(input.options, "header", LogicalType::BOOLEAN);
}

BoundStatement ExcelTableCopyPlan(Binder &binder, CopyStatement &stmt) {
	return RewriteCopyInPlace(binder, stmt, kTableFormatInternal, "workbook", true);
}

BoundStatement ExcelRangeCopyPlan(Binder &binder, CopyStatement &stmt) {
	return RewriteCopyInPlace(binder, stmt, kRangeFormatInternal, "workbook", true);
}

CopyFunction MakeExcelWriter(const char *name, copy_to_bind_t bind, copy_options_t options) {
	CopyFunction fn(duckdb_compat::FunctionName(name));
	fn.copy_options = options;
	fn.copy_to_bind = bind;
	fn.copy_to_initialize_global = ExcelWriteInitGlobal;
	fn.copy_to_initialize_local = ExcelWriteInitLocal;
	fn.copy_to_sink = ExcelWriteSink;
	fn.copy_to_combine = ExcelWriteCombine;
	fn.copy_to_finalize = ExcelWriteFinalize;
	fn.execution_mode = ExcelWriteExecutionMode;
	fn.extension = "xlsx";
	return fn;
}

CopyFunction MakeExcelFrontend(const char *name, copy_to_plan_t plan) {
	CopyFunction fn(duckdb_compat::FunctionName(name));
	fn.plan = plan;
	fn.extension = "xlsx";
	return fn;
}

} // namespace

// --- For an ATTACHed site's workbooks (#100) ----------------------------------------

vector<string> ExcelWorkbookTables(ClientContext &context, const string &path) {
	auto wb = OpenWorkbook(context, path);
	GraphClient client(wb.auth);
	const string what = "the tables of '" + wb.path + "'";
	const auto tables = ParseObject(client.WorkbookGet(wb.item_id, "tables?$select=name", what), what);
	vector<string> names;
	if (auto values = ArrayField(tables, "value")) {
		for (auto &t : *values) {
			if (t.is<picojson::object>()) {
				auto name = t.get<picojson::object>().find("name");
				if (name != t.get<picojson::object>().end() && name->second.is<string>()) {
					names.push_back(name->second.get<string>());
				}
			}
		}
	}
	return names;
}

unique_ptr<FunctionData> BindExcelTableForCatalog(ClientContext &context, const string &path, const string &table,
                                                  vector<string> &names, vector<LogicalType> &types) {
	duckdb_compat::BindNames bound;
	auto data = BindTable(context, path, table, false, types, bound);
	for (auto &n : bound) {
		names.push_back(duckdb_compat::Str(n));
	}
	return data;
}

TableFunction ExcelTableScanFunction() {
	return TableFunction("read_excel_table", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ExcelScan, TableBind,
	                     CursorInit);
}

struct ExcelTableAppender::State {
	ExcelWriteBindData bind;
	unique_ptr<GlobalFunctionData> global;
	LocalFunctionData local;
};

ExcelTableAppender::ExcelTableAppender(ClientContext &context, const string &path, const string &table,
                                       const vector<string> &names, const vector<LogicalType> &types)
    : state(make_uniq<State>()) {
	state->bind.table_mode = true;
	state->bind.table = table;
	state->bind.sheet = table;
	state->bind.names = names;
	for (auto &t : types) {
		state->bind.kinds.push_back(WriteKindFor(t));
	}
	state->global = ExcelWriteInitGlobal(context, state->bind, path);
}

ExcelTableAppender::~ExcelTableAppender() {
}

void ExcelTableAppender::Append(ExecutionContext &context, DataChunk &chunk) {
	ExcelWriteSink(context, state->bind, *state->global, state->local, chunk);
	written += chunk.size();
}

void ExcelTableAppender::Finish(ClientContext &context) {
	ExcelWriteFinalize(context, state->bind, *state->global);
}

void RegisterExcelFunctions(ExtensionLoader &loader) {
	Register(loader,
	         TableFunction("read_excel_table", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ExcelScan, TableBind,
	                       CursorInit),
	         {"path", "table"},
	         "Read a named Excel table from an .xlsx in SharePoint / OneDrive, live through the Graph workbook API: "
	         "the header row gives the column names, the values and number formats the types (dates included). "
	         "The path is a sharepoint:// path (the scheme may be left out) or id:<itemId>. all_varchar := true "
	         "reads every column as text.",
	         {"SELECT * FROM read_excel_table('Finance/plan.xlsx', 'Sales')"});

	TableFunction range("read_excel_range", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ExcelScan, RangeBind,
	                    CursorInit);
	duckdb_compat::AddNamedParameter(range, "header", LogicalType::BOOLEAN);
	Register(loader, range, {"path", "range"},
	         "Read a range of an .xlsx in SharePoint / OneDrive, live through the Graph workbook API: 'Sheet!A1:F200', "
	         "or a sheet name for its used range. The first row is the header unless header := false. "
	         "all_varchar := true reads every column as text.",
	         {"SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input!A1:F200')",
	          "SELECT * FROM read_excel_range('Finance/plan.xlsx', 'Input', header := false)"});

	loader.RegisterFunction(MakeExcelFrontend(kTableFormat, ExcelTableCopyPlan));
	loader.RegisterFunction(MakeExcelFrontend(kRangeFormat, ExcelRangeCopyPlan));
	loader.RegisterFunction(MakeExcelWriter(kTableFormatInternal, ExcelTableWriteBind, ExcelTableCopyOptions));
	loader.RegisterFunction(MakeExcelWriter(kRangeFormatInternal, ExcelRangeWriteBind, ExcelRangeCopyOptions));
}

} // namespace sharepoint
} // namespace duckdb
