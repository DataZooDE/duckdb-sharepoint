#include "sharepoint_vfs_functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

// int64_t is used below. libstdc++ leaks it transitively, musl's libc++ does
// not, and musl is in the 1.4 LTS build matrix.
#include <cstdint>
#include <utility>

namespace duckdb {
namespace sharepoint {

namespace {

//! Confirm that `path` is genuinely ABSENT rather than unreachable.
//!
//! `FileExists` returning false is not proof of absence: a filesystem answers
//! false when no secret is configured or the URI does not parse, because
//! DuckDB core probes speculatively during replacement-scan binding and would
//! break if that threw. Building a user-facing "does not exist" on that
//! leniency would report a CONFIGURATION error as a missing file.
//!
//! Opening with NULL_IF_NOT_EXISTS is the strict question: null means the
//! filesystem looked and found nothing; anything else throws.
bool ConfirmedAbsent(FileSystem &fs, const string &path) {
	auto probe = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
	return probe == nullptr;
}

bool RemoveOne(FileSystem &fs, const string &path) {
	if (!fs.FileExists(path)) {
		if (fs.DirectoryExists(path)) {
			throw IOException("'%s' is a directory, not a file; remove_file does not remove directories", path);
		}
		if (ConfirmedAbsent(fs, path)) {
			return false;
		}
		throw IOException("'%s' exists but is not a removable file", path);
	}
	fs.RemoveFile(path);
	return true;
}

void RemoveFileScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(),
	                                       [&](string_t path) { return RemoveOne(fs, path.GetString()); });
}

void MoveFileScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());
	BinaryExecutor::Execute<string_t, string_t, bool>(args.data[0], args.data[1], result, args.size(),
	                                                  [&](string_t source, string_t target) {
		                                                  fs.MoveFile(source.GetString(), target.GetString());
		                                                  return true;
	                                                  });
}

// Create every missing directory above `target`, shallowest first. Walks
// DOWNWARD from the scheme, which is scheme-agnostic -- FileSystem::
// CreateDirectoriesRecursive walks upward with Path::Parent(), which does not
// understand a URL-form path and stops after one level.
void EnsureParentDirectories(FileSystem &fs, const string &target) {
	auto last_sep = target.find_last_of('/');
	if (last_sep == string::npos) {
		return;
	}
	size_t start = 0;
	auto scheme = target.find("://");
	if (scheme != string::npos) {
		start = scheme + 3;
	} else if (!target.empty() && target.front() == '/') {
		start = 1;
	}
	for (auto i = target.find('/', start); i != string::npos && i <= last_sep; i = target.find('/', i + 1)) {
		auto dir = target.substr(0, i);
		if (dir.empty()) {
			continue;
		}
		if (!fs.DirectoryExists(dir)) {
			if (fs.FileExists(dir)) {
				throw IOException("sharepoint: cannot create directory '%s': a file of that name already "
				                  "exists.",
				                  dir);
			}
			fs.CreateDirectory(dir);
		}
	}
}

// The inverse of core's read_blob(). FILE_CREATE_NEW gives overwrite
// semantics; FILE_CREATE would leave a tail of longer previous content behind.
// A single Write() call matters on sharepoint://, whose handle uploads on
// close: one call is one upload, so the file appears complete or not at all.
void WriteBlobScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());
	BinaryExecutor::Execute<string_t, string_t, int64_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, string_t content) {
		    auto target = path.GetString();
		    EnsureParentDirectories(fs, target);
		    auto handle =
		        fs.OpenFile(target, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		    if (!handle) {
			    throw IOException("write_blob: could not open '%s' for writing", target);
		    }
		    auto size = NumericCast<int64_t>(content.GetSize());
		    if (size > 0) {
			    handle->Write(const_cast<char *>(content.GetDataUnsafe()), size);
		    }
		    handle->Sync();
		    handle->Close();
		    return size;
	    });
}

// Size WITHOUT transferring the body -- one metadata call, not a full download.
void FileSizeScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());
	UnaryExecutor::ExecuteWithNulls<string_t, int64_t>(
	    args.data[0], result, args.size(), [&](string_t path, ValidityMask &mask, idx_t idx) -> int64_t {
		    auto p = path.GetString();
		    if (!fs.FileExists(p)) {
			    if (fs.DirectoryExists(p)) {
				    throw IOException("'%s' is a directory, not a file; it has no byte size", p);
			    }
			    if (ConfirmedAbsent(fs, p)) {
				    mask.SetInvalid(idx);
				    return 0;
			    }
			    throw IOException("'%s' exists but its size cannot be read as a file", p);
		    }
		    auto handle = fs.OpenFile(p, FileFlags::FILE_FLAGS_READ);
		    if (!handle) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return NumericCast<int64_t>(handle->GetFileSize());
	    });
}

} // namespace

// The four functions are scheme-generic and shared, byte for byte in
// behaviour, with the sibling gdrive extension, which registers them too.
// Registering with IGNORE_ON_CONFLICT lets both extensions load into one
// database in either order: whichever loads second keeps the first's
// (identical) definitions. Without it the second LOAD failed with
// 'Scalar Function with name "remove_file" already exists!'.
void RegisterVfsFunctions(ExtensionLoader &loader) {
	{
		ScalarFunction fn("remove_file", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, RemoveFileScalar);
		// A delete is not a pure function of its argument: the same call twice
		// returns true then false. VOLATILE keeps the optimiser from folding it.
		fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo info(fn);
		FunctionDescription desc;
		desc.description =
		    "Delete the file at `path`, returning true if it existed and was removed and false if it did "
		    "not exist. Dispatches on the path's scheme through DuckDB's virtual filesystem, so it works "
		    "for sharepoint://, s3://, gs:// and local paths alike. For a sharepoint:// path the item is "
		    "moved to the recycle bin unless sharepoint_permanent_delete is set. Errors other than "
		    "not-found are raised, not returned as false.";
		desc.parameter_names = {"path"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT remove_file('sharepoint://reports/old.parquet')"};
		desc.categories = {"sharepoint"};
		info.descriptions.push_back(std::move(desc));
		info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	{
		ScalarFunction fn("move_file", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
		                  MoveFileScalar);
		fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo info(fn);
		FunctionDescription desc;
		desc.description =
		    "Rename/move `source` to `target`, returning true on success and raising on failure. Both paths "
		    "must live on the SAME filesystem -- this is a rename, not a copy. Its main use is publishing a "
		    "fully-written temporary file under its final name.";
		desc.parameter_names = {"source", "target"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT move_file('sharepoint://staging/part.tmp', 'sharepoint://data/part.parquet')"};
		desc.categories = {"sharepoint"};
		info.descriptions.push_back(std::move(desc));
		info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	{
		ScalarFunction write_fn("write_blob", {LogicalType::VARCHAR, LogicalType::BLOB}, LogicalType::BIGINT,
		                        WriteBlobScalar);
		write_fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo write_info(write_fn);
		FunctionDescription write_desc;
		write_desc.description =
		    "Write `content` to `path`, replacing any existing file, and return the number of bytes "
		    "written. The inverse of read_blob(): together they give SQL a byte-exact round trip for any "
		    "filesystem DuckDB can reach, including sharepoint://. Accepts arbitrary binary content -- a "
		    "BLOB, not a VARCHAR.";
		write_desc.parameter_names = {"path", "content"};
		write_desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BLOB};
		write_desc.examples = {"SELECT write_blob('sharepoint://notes/readme.md', '# Title'::BLOB)"};
		write_desc.categories = {"sharepoint"};
		write_info.descriptions.push_back(std::move(write_desc));
		write_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		loader.RegisterFunction(std::move(write_info));
	}

	{
		ScalarFunction size_fn("file_size", {LogicalType::VARCHAR}, LogicalType::BIGINT, FileSizeScalar);
		size_fn.stability = FunctionStability::VOLATILE;
		CreateScalarFunctionInfo size_info(size_fn);
		FunctionDescription size_desc;
		size_desc.description =
		    "Byte length of the file at `path`, or NULL if it does not exist. Reads only metadata -- unlike "
		    "read_blob, which downloads the body.";
		size_desc.parameter_names = {"path"};
		size_desc.parameter_types = {LogicalType::VARCHAR};
		size_desc.examples = {"SELECT file_size('sharepoint://data/part.parquet')"};
		size_desc.categories = {"sharepoint"};
		size_info.descriptions.push_back(std::move(size_desc));
		size_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		loader.RegisterFunction(std::move(size_info));
	}
}

} // namespace sharepoint
} // namespace duckdb
