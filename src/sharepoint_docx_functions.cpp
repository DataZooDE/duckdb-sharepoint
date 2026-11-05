// read_docx(path) and read_docx_table(path, n) (#103): Word documents as
// rows, read as files through DuckDB's filesystem (sharepoint://, a browser
// address, or any path), unzipped with sharepoint_excel's Zip* helpers and
// parsed by sharepoint_docx (pure). No Graph Word API.
//
// COPY ... TO 'x.docx' (FORMAT docx, TEMPLATE 't.docx') (#104): a template
// filled from the query (sharepoint_docx's FillDocxPart, pure) and written as
// a new file -- there is no Graph API for editing Word documents.
#include "sharepoint_docx_functions.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_copy_rewrite.hpp"
#include "sharepoint_docx.hpp"
#include "sharepoint_excel.hpp"

#include "duckdb.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/function/copy_function.hpp"
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

// --- COPY ... TO 'x.docx' (FORMAT docx, TEMPLATE 't.docx') ------------------

struct DocxWriteBindData : public FunctionData {
	string template_path;
	DocxValues variables; // VARIABLES {...}: win over the first row's columns
	vector<string> names;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DocxWriteBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DocxWriteBindData>();
		return template_path == other.template_path && variables == other.variables && names == other.names;
	}
};

//! A value as the document shows it: its text, NULL as nothing.
string DocxText(const Value &v) {
	return v.IsNull() ? string() : v.ToString();
}

unique_ptr<FunctionData> DocxWriteBind(ClientContext &, CopyFunctionBindInput &input,
                                       const duckdb_compat::CopyBindNames &names, const vector<LogicalType> &) {
	auto &info = input.info;
	auto data = make_uniq<DocxWriteBindData>();
	auto tmpl = FindCopyOption(info.options, "template");
	if (!tmpl || tmpl->size() != 1 || (*tmpl)[0].IsNull()) {
		throw InvalidInputException("sharepoint: FORMAT docx needs TEMPLATE '<a .docx>' to fill");
	}
	data->template_path = (*tmpl)[0].ToString();
	if (auto vars = FindCopyOption(info.options, "variables")) {
		if (vars->size() != 1) {
			throw InvalidInputException("sharepoint: VARIABLES takes one STRUCT, e.g. {'customer': 'ACME'}");
		}
		auto &v = (*vars)[0];
		if (v.type().id() == LogicalTypeId::STRUCT) {
			auto &children = StructValue::GetChildren(v);
			for (idx_t i = 0; i < children.size(); i++) {
				data->variables[DocxKey(duckdb_compat::Str(StructType::GetChildName(v.type(), i)))] =
				    DocxText(children[i]);
			}
		} else if (v.type().id() == LogicalTypeId::MAP) {
			for (auto &entry : MapValue::GetChildren(v)) {
				auto &kv = StructValue::GetChildren(entry);
				data->variables[DocxKey(kv[0].ToString())] = DocxText(kv[1]);
			}
		} else {
			throw InvalidInputException("sharepoint: VARIABLES takes a STRUCT, e.g. {'customer': 'ACME'}, not %s",
			                            v.type().ToString());
		}
	}
	for (auto &n : names) {
		data->names.push_back(duckdb_compat::Str(n));
	}
	return std::move(data);
}

struct DocxWriteGlobalState : public GlobalFunctionData {
	string file_path;
	vector<DocxValues> rows;
};

unique_ptr<GlobalFunctionData> DocxWriteInitGlobal(ClientContext &, FunctionData &, const string &file_path) {
	auto g = make_uniq<DocxWriteGlobalState>();
	g->file_path = file_path;
	return std::move(g);
}

unique_ptr<LocalFunctionData> DocxWriteInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

void DocxWriteSink(ExecutionContext &, FunctionData &bind_p, GlobalFunctionData &gstate_p, LocalFunctionData &,
                   DataChunk &input) {
	auto &bind = bind_p.Cast<DocxWriteBindData>();
	auto &g = gstate_p.Cast<DocxWriteGlobalState>();
	for (idx_t r = 0; r < input.size(); r++) {
		DocxValues row;
		for (idx_t c = 0; c < input.ColumnCount(); c++) {
			row[DocxKey(bind.names[c])] = DocxText(input.GetValue(c, r));
		}
		g.rows.push_back(std::move(row));
	}
}

void DocxWriteCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

string ReadWhole(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	const auto size = static_cast<uint64_t>(handle->GetFileSize());
	if (size > kMaxDocxBytes) {
		throw InvalidInputException("sharepoint: '%s' is larger than %llu MB", path,
		                            (unsigned long long)(kMaxDocxBytes >> 20));
	}
	string bytes(size, '\0');
	handle->Read(&bytes[0], size, 0);
	return bytes;
}

void DocxWriteFinalize(ClientContext &context, FunctionData &bind_p, GlobalFunctionData &gstate_p) {
	auto &bind = bind_p.Cast<DocxWriteBindData>();
	auto &g = gstate_p.Cast<DocxWriteGlobalState>();
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		const auto &path = bind.template_path;
		vector<ZipMember> members;
		string error;
		if (!ReadZipMembers(ReadWhole(context, path), members, error)) {
			throw InvalidInputException("sharepoint: the template '%s' is not a Word document (.docx): %s", path,
			                            error);
		}
		DocxFillInput input;
		if (!g.rows.empty()) {
			input.scalars = g.rows[0];
		}
		for (auto &v : bind.variables) {
			input.scalars[v.first] = v.second;
		}
		input.rows = std::move(g.rows);
		bool has_body = false, repeated = false;
		for (auto &m : members) {
			if (!IsDocxTemplatePart(m.name)) {
				continue;
			}
			has_body = has_body || m.name == "word/document.xml";
			if (m.method != 0 && m.method != 8) {
				throw InvalidInputException("sharepoint: the template '%s' compresses %s in a way not supported", path,
				                            m.name);
			}
			auto xml = m.method == 8 ? GZipFileSystem::UncompressGZIPString(
			                               GzipFrame(m.data, m.crc32, static_cast<uint32_t>(m.uncompressed_size)))
			                         : m.data;
			string filled;
			if (!FillDocxPart(xml, input, filled, repeated, error)) {
				throw InvalidInputException("sharepoint: the template '%s' (%s): %s", path, m.name, error);
			}
			if (filled != xml) {
				m.method = 0;
				m.crc32 = ZipCrc32(filled);
				m.uncompressed_size = filled.size();
				m.data = std::move(filled);
			}
		}
		if (!has_body) {
			throw InvalidInputException("sharepoint: the template '%s' is not a Word document (.docx)", path);
		}
		if (!repeated && input.rows.size() > 1) {
			throw InvalidInputException(
			    "sharepoint: the query returned %llu rows, but the template '%s' has no table row marked {{#rows}} "
			    "to repeat for them",
			    (unsigned long long)input.rows.size(), path);
		}
		const auto zip = WriteZip(members);
		auto &fs = FileSystem::GetFileSystem(context);
		auto out = fs.OpenFile(g.file_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		out->Write(const_cast<char *>(zip.data()), zip.size());
		// Closed here, so a SharePoint file is published before COPY moves its
		// temporary file over an existing target.
		out->Close();
	});
}

CopyFunctionExecutionMode DocxWriteExecutionMode(bool, bool) {
	// One writer, rows in query order: they become the repeated rows in order.
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

void DocxCopyOptions(ClientContext &, CopyOptionsInput &input) {
	AddCopyOption(input.options, "template", LogicalType::VARCHAR);
	AddCopyOption(input.options, "variables", LogicalType::ANY);
}

} // namespace

void RegisterDocxFunctions(ExtensionLoader &loader) {
	CopyFunction writer(duckdb_compat::FunctionName("docx"));
	writer.copy_options = DocxCopyOptions;
	writer.copy_to_bind = DocxWriteBind;
	writer.copy_to_initialize_global = DocxWriteInitGlobal;
	writer.copy_to_initialize_local = DocxWriteInitLocal;
	writer.copy_to_sink = DocxWriteSink;
	writer.copy_to_combine = DocxWriteCombine;
	writer.copy_to_finalize = DocxWriteFinalize;
	writer.execution_mode = DocxWriteExecutionMode;
	writer.extension = "docx";
	loader.RegisterFunction(writer);

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
