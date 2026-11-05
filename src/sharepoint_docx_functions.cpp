// read_docx(path) and read_docx_table(path, n) (#103): Word documents as
// rows, read as files through DuckDB's filesystem (sharepoint://, a browser
// address, or any path), unzipped with sharepoint_excel's Zip* helpers and
// parsed by sharepoint_docx (pure). No Graph Word API.
#include "sharepoint_docx_functions.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_docx.hpp"
#include "sharepoint_excel.hpp"

#include "duckdb.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <cstdint>

#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

constexpr idx_t kMaxDocxBytes = 64ULL * 1024 * 1024;

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

string ArgString(TableFunctionBindInput &input, idx_t i, const char *what) {
	if (input.inputs.size() <= i || input.inputs[i].IsNull()) {
		throw InvalidInputException("sharepoint: the %s must not be NULL", what);
	}
	return StringValue::Get(input.inputs[i]);
}

//! The files `path` names: itself, or what its glob matches.
vector<string> Files(ClientContext &context, const string &path) {
	// The context's filesystem brings its own opener (passing one is an error).
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> files;
	if (!FileSystem::HasGlob(path)) {
		files.push_back(path);
		return files;
	}
	for (auto &f : fs.Glob(path)) {
		files.push_back(f.path);
	}
	if (files.empty()) {
		throw InvalidInputException("sharepoint: no file matches '%s'", path);
	}
	return files;
}

//! word/document.xml of the .docx at `path`, parsed.
DocxBody ReadDocx(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	const auto size = static_cast<uint64_t>(handle->GetFileSize());
	if (size > kMaxDocxBytes) {
		throw InvalidInputException("sharepoint: '%s' is larger than %llu MB", path,
		                            (unsigned long long)(kMaxDocxBytes >> 20));
	}
	string bytes(size, '\0');
	handle->Read(&bytes[0], size, 0);
	uint64_t cd_offset, cd_size;
	ZipEntry entry;
	uint64_t header = 0;
	if (!ZipCentralDirectory(bytes, size, cd_offset, cd_size) || cd_offset + cd_size > size ||
	    !ZipFindEntry(bytes.substr(cd_offset, cd_size), "word/document.xml", entry) ||
	    (entry.method != 0 && entry.method != 8) || entry.local_header_offset >= size ||
	    !ZipLocalHeaderSize(bytes.substr(entry.local_header_offset), header) ||
	    entry.local_header_offset + header + entry.compressed_size > size) {
		throw InvalidInputException("sharepoint: '%s' is not a Word document (.docx)", path);
	}
	auto xml = bytes.substr(entry.local_header_offset + header, entry.compressed_size);
	if (entry.method == 8) {
		xml = GZipFileSystem::UncompressGZIPString(
		    GzipFrame(xml, entry.crc32, static_cast<uint32_t>(entry.uncompressed_size)));
	}
	DocxBody body;
	string error;
	if (!ParseDocx(xml, body, error)) {
		throw InvalidInputException("sharepoint: '%s': %s", path, error);
	}
	return body;
}

unique_ptr<FunctionData> DocxBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto data = make_uniq<RowsBindData>();
		for (auto &file : Files(context, ArgString(input, 0, "path"))) {
			auto body = ReadDocx(context, file);
			for (idx_t i = 0; i < body.paragraphs.size(); i++) {
				auto &p = body.paragraphs[i];
				data->rows.push_back({Value(file), Value::BIGINT(static_cast<int64_t>(i + 1)),
				                      p.style.empty() ? Value(LogicalType::VARCHAR) : Value(p.style),
				                      p.heading_level ? Value::INTEGER(p.heading_level) : Value(LogicalType::INTEGER),
				                      Value(p.text)});
			}
		}
		names = {"file", "idx", "style", "heading_level", "text"};
		return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::INTEGER,
		                LogicalType::VARCHAR};
		result = std::move(data);
	});
	return result;
}

unique_ptr<FunctionData> DocxTableBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	unique_ptr<FunctionData> result;
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto path = ArgString(input, 0, "path");
		int64_t n = 1;
		if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
			n = input.inputs[1].GetValue<int64_t>();
		}
		auto body = ReadDocx(context, path);
		if (n < 1 || static_cast<size_t>(n) > body.tables.size()) {
			throw InvalidInputException("sharepoint: '%s' has %llu table(s); there is no table %lld", path,
			                            (unsigned long long)body.tables.size(), (long long)n);
		}
		auto &rows = body.tables[static_cast<size_t>(n - 1)].rows;
		size_t width = 0;
		for (auto &r : rows) {
			width = std::max(width, r.size());
		}
		// The first row names the columns: blank or repeated names get a number.
		case_insensitive_set_t used;
		for (size_t c = 0; c < width; c++) {
			string name = !rows.empty() && c < rows[0].size() ? rows[0][c] : string();
			if (name.empty()) {
				name = "column" + std::to_string(c + 1);
			}
			string unique = name;
			for (int k = 2; used.count(unique); k++) {
				unique = name + "_" + std::to_string(k);
			}
			used.insert(unique);
			names.emplace_back(unique);
			return_types.push_back(LogicalType::VARCHAR);
		}
		auto data = make_uniq<RowsBindData>();
		for (size_t r = 1; r < rows.size(); r++) {
			vector<Value> row;
			for (size_t c = 0; c < width; c++) {
				row.push_back(c < rows[r].size() ? Value(rows[r][c]) : Value(LogicalType::VARCHAR));
			}
			data->rows.push_back(std::move(row));
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

void Register(ExtensionLoader &loader, TableFunctionSet set, const string &description, vector<string> examples) {
	CreateTableFunctionInfo info(std::move(set));
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"sharepoint"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterDocxFunctions(ExtensionLoader &loader) {
	TableFunctionSet docx("read_docx");
	docx.AddFunction(TableFunction("read_docx", {LogicalType::VARCHAR}, RowsScan, DocxBind, CursorInit));
	Register(loader, docx,
	         "The paragraphs of Word documents (.docx; a path or glob, sharepoint:// or any filesystem): file, idx "
	         "(1-based), style, heading_level (1-9 for Heading1..9) and text. Paragraphs inside tables are left to "
	         "read_docx_table.",
	         {"SELECT file, text FROM read_docx('sharepoint://Contracts/*.docx') WHERE text ILIKE '%notice period%'"});

	TableFunctionSet table("read_docx_table");
	table.AddFunction(TableFunction("read_docx_table", {LogicalType::VARCHAR}, RowsScan, DocxTableBind, CursorInit));
	table.AddFunction(TableFunction("read_docx_table", {LogicalType::VARCHAR, LogicalType::BIGINT}, RowsScan,
	                                DocxTableBind, CursorInit));
	Register(loader, table,
	         "The n-th table (default 1) of a Word document (.docx): its first row names the columns, every value "
	         "is text (a cell's paragraphs joined by newlines).",
	         {"SELECT * FROM read_docx_table('sharepoint://Reports/q4.docx', 2)"});
}

} // namespace sharepoint
} // namespace duckdb
