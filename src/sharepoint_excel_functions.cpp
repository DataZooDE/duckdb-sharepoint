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
#include "sharepoint_excel.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
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

//! Cells per range request. Graph answers a range with three parallel arrays
//! (values, valueTypes, numberFormat) and rejects or times out on very large
//! ones; 20k cells is a few MB of JSON. A 10,000 x 5 table is three requests.
constexpr int64_t kCellsPerRequest = 20000;

struct ExcelBindData : public TableFunctionData {
	vector<ExcelColumnType> types;
	//! Row-major. Read whole at bind: the column types depend on every row.
	vector<vector<ExcelCell>> rows;
};

//! The workbook an argument names, and what is needed to call Graph for it.
struct Workbook {
	AuthContext auth;
	string item_id;
	string path; // as the user can paste it back, for messages
};

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

Value ToValue(const ExcelCell &cell, ExcelColumnType t) {
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
	switch (t) {
	case ExcelColumnType::BIGINT:
		return Value::BIGINT(static_cast<int64_t>(cell.number));
	case ExcelColumnType::DOUBLE:
		return Value::DOUBLE(cell.number);
	case ExcelColumnType::DATE:
		ExcelSerialToEpoch(cell.number, days, micros);
		return Value::DATE(date_t(static_cast<int32_t>(days)));
	case ExcelColumnType::TIMESTAMP:
		ExcelSerialToEpoch(cell.number, days, micros);
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
unique_ptr<FunctionData> Finish(vector<string> names, vector<vector<ExcelCell>> rows, bool all_varchar,
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
	return std::move(data);
}

unique_ptr<FunctionData> TableBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto table = StringArg(input, 1, "table name");
		const bool all_varchar = BoolOption(input, "all_varchar", false);
		auto wb = OpenWorkbook(context, StringArg(input, 0, "workbook path"));
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
		    Finish(ExcelHeaderNames(header_cells), ReadCells(client, wb, address), all_varchar, return_types, names);
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
		result = Finish(ExcelHeaderNames(header_cells), std::move(rows), all_varchar, return_types, names);
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
			output.SetValue(c, out, ToValue(row[c], data.types[c]));
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

} // namespace

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
}

} // namespace sharepoint
} // namespace duckdb
