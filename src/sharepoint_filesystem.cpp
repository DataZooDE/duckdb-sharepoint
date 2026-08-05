// The sharepoint:// virtual filesystem.
//
// STATUS: network frontier of the scaffold (see sharepoint_auth.cpp's header).
// The DuckDB FileSystem method surface is complete and the Graph mapping is
// implemented, but nothing here has run against a live tenant yet.
#include "sharepoint_filesystem.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

namespace duckdb {
namespace sharepoint {

namespace {

SharepointUri ParseOrThrow(const string &path) {
	auto parsed = ParseSharepointUri(path);
	if (!parsed.ok) {
		throw IOException("sharepoint: %s", parsed.error);
	}
	return parsed.uri;
}

ClientContext &RequireContext(optional_ptr<FileOpener> opener, const char *op) {
	auto context = FileOpener::TryGetClientContext(opener.get());
	if (!context) {
		throw IOException("sharepoint: %s needs an active connection context but none was available", op);
	}
	return *context;
}

idx_t UploadChunkBytes(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("sharepoint_upload_chunk_bytes", v) && !v.IsNull()) {
		return v.GetValue<idx_t>();
	}
	return 10ULL * 1024 * 1024;
}

bool PermanentDelete(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("sharepoint_permanent_delete", v) && !v.IsNull()) {
		return v.GetValue<bool>();
	}
	return false;
}

} // namespace

SharepointFileHandle::SharepointFileHandle(FileSystem &fs, string path, FileOpenFlags flags, AuthContext auth)
    : FileHandle(fs, std::move(path), flags), auth(std::move(auth)) {
}

void SharepointFileHandle::Close() {
	if (!is_write || write_failed) {
		return;
	}
	// Publish the buffered bytes as one upload. A zero-length write still
	// creates an empty file, matching local-fs semantics. PutContent chooses a
	// single PUT vs. a resumable session by comparing the buffer to
	// upload_chunk_bytes (captured at open time).
	GraphClient client(auth);
	client.PutContent(write_root_relative, write_buffer, upload_chunk_bytes);
	is_write = false;
}

bool SharepointFileSystem::CanHandleFile(const string &fpath) {
	return IsSharepointUri(fpath);
}

unique_ptr<FileHandle> SharepointFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                      optional_ptr<FileOpener> opener) {
	auto uri = ParseOrThrow(path);
	auto &context = RequireContext(opener, "open");
	auto auth = GetAuthContext(context, path);

	if (flags.OpenForWriting()) {
		if (uri.kind != SharepointUriKind::PATH) {
			throw IOException("sharepoint: writing addresses a path, not an id: form ('%s')", path);
		}
		auto handle = make_uniq<SharepointFileHandle>(*this, path, flags, std::move(auth));
		handle->is_write = true;
		handle->write_root_relative = uri.RootRelativePath();
		handle->upload_chunk_bytes = UploadChunkBytes(context);
		return std::move(handle);
	}

	// Read: resolve metadata once.
	GraphClient client(auth);
	DriveItemMeta meta = (uri.kind == SharepointUriKind::FILE_ID) ? client.StatById(uri.item_id)
	                                                              : client.StatByPath(uri.RootRelativePath());
	if (!meta.found || meta.is_folder) {
		if (flags.ReturnNullIfNotExists()) {
			return nullptr;
		}
		throw IOException("sharepoint: no such file: '%s'", path);
	}
	auto handle = make_uniq<SharepointFileHandle>(*this, path, flags, std::move(auth));
	handle->meta = std::move(meta);
	return std::move(handle);
}

void SharepointFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = handle.Cast<SharepointFileHandle>();
	if (nr_bytes <= 0) {
		return;
	}
	GraphClient client(h.auth);
	string chunk;
	client.ReadRange(h.meta.id, static_cast<int64_t>(location), nr_bytes, chunk);
	if (static_cast<int64_t>(chunk.size()) < nr_bytes) {
		throw IOException("sharepoint: short read of '%s' (wanted %lld, got %llu)", handle.path.c_str(),
		                  (long long)nr_bytes, (unsigned long long)chunk.size());
	}
	std::memcpy(buffer, chunk.data(), nr_bytes);
}

int64_t SharepointFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<SharepointFileHandle>();
	int64_t remaining = static_cast<int64_t>(h.meta.size) - static_cast<int64_t>(h.position);
	if (remaining <= 0) {
		return 0;
	}
	int64_t to_read = MinValue<int64_t>(nr_bytes, remaining);
	Read(handle, buffer, to_read, h.position);
	h.position += to_read;
	return to_read;
}

void SharepointFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	// Positional write: Graph replaces whole files and cannot write at an
	// offset. Fail loudly rather than silently drop or corrupt.
	throw IOException("sharepoint: cannot write at a byte offset -- Graph replaces whole files. Write "
	                  "sequentially (the file is uploaded in full on close).");
}

int64_t SharepointFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<SharepointFileHandle>();
	if (!h.is_write) {
		throw IOException("sharepoint: '%s' was not opened for writing", handle.path.c_str());
	}
	h.write_buffer.append(static_cast<const char *>(buffer), static_cast<size_t>(nr_bytes));
	h.position += nr_bytes;
	return nr_bytes;
}

int64_t SharepointFileSystem::GetFileSize(FileHandle &handle) {
	auto &h = handle.Cast<SharepointFileHandle>();
	return h.is_write ? static_cast<int64_t>(h.write_buffer.size()) : h.meta.size;
}

string SharepointFileSystem::GetVersionTag(FileHandle &handle) {
	auto &h = handle.Cast<SharepointFileHandle>();
	return h.meta.ctag; // content tag: changes on every content change
}

bool SharepointFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	// Speculative probes during binding may arrive with no opener; answer
	// false rather than throw (the leniency sharepoint_vfs_functions.cpp's
	// ConfirmedAbsent accounts for).
	auto context = FileOpener::TryGetClientContext(opener.get());
	if (!context) {
		return false;
	}
	auto parsed = ParseSharepointUri(filename);
	if (!parsed.ok) {
		return false;
	}
	auto auth = GetAuthContext(*context, filename);
	GraphClient client(auth);
	auto meta = (parsed.uri.kind == SharepointUriKind::FILE_ID) ? client.StatById(parsed.uri.item_id)
	                                                            : client.StatByPath(parsed.uri.RootRelativePath());
	return meta.found && !meta.is_folder;
}

bool SharepointFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener.get());
	if (!context) {
		return false;
	}
	auto parsed = ParseSharepointUri(directory);
	if (!parsed.ok || parsed.uri.kind == SharepointUriKind::FILE_ID) {
		return false;
	}
	auto auth = GetAuthContext(*context, directory);
	GraphClient client(auth);
	auto meta = client.StatByPath(parsed.uri.RootRelativePath());
	return meta.found && meta.is_folder;
}

void SharepointFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	auto uri = ParseOrThrow(directory);
	auto &context = RequireContext(opener, "create directory");
	auto auth = GetAuthContext(context, directory);
	GraphClient(auth).CreateFolder(uri.RootRelativePath());
}

void SharepointFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	// A directory is a driveItem too; resolve and delete it (recursively, as
	// Graph deletes a folder and its contents).
	auto uri = ParseOrThrow(directory);
	auto &context = RequireContext(opener, "remove directory");
	auto auth = GetAuthContext(context, directory);
	GraphClient client(auth);
	auto meta = client.StatByPath(uri.RootRelativePath());
	if (meta.found) {
		client.DeleteItem(meta.id, PermanentDelete(context));
	}
}

bool SharepointFileSystem::ListFiles(const string &directory,
                                     const std::function<void(const string &, bool)> &callback,
                                     FileOpener *opener) {
	auto uri = ParseOrThrow(directory);
	auto &context = RequireContext(opener, "list files");
	auto auth = GetAuthContext(context, directory);
	auto children = GraphClient(auth).ListChildren(uri.RootRelativePath());
	for (auto &c : children) {
		callback(c.name, c.is_folder);
	}
	return !children.empty();
}

void SharepointFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto src = ParseOrThrow(source);
	auto dst = ParseOrThrow(target);
	if (src.kind != SharepointUriKind::PATH || dst.kind != SharepointUriKind::PATH) {
		throw IOException("sharepoint: move addresses paths on both sides, not id: forms");
	}
	auto &context = RequireContext(opener, "move");
	auto auth = GetAuthContext(context, source);
	GraphClient(auth).MoveByPath(src.RootRelativePath(), dst.RootRelativePath());
}

void SharepointFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto uri = ParseOrThrow(filename);
	auto &context = RequireContext(opener, "remove");
	auto auth = GetAuthContext(context, filename);
	GraphClient client(auth);
	string id;
	if (uri.kind == SharepointUriKind::FILE_ID) {
		id = uri.item_id;
	} else {
		auto meta = client.StatByPath(uri.RootRelativePath());
		if (!meta.found) {
			return; // already gone
		}
		id = meta.id;
	}
	client.DeleteItem(id, PermanentDelete(context));
}

vector<OpenFileInfo> SharepointFileSystem::Glob(const string &path, FileOpener *opener) {
	vector<OpenFileInfo> result;
	// Core synthesizes "sharepoint://id:<id>/**/*.<ext>" when a literal path
	// resolves to nothing; an id: form is never a directory, so the honest
	// answer is empty rather than the id:-cannot-have-segments error.
	if (IsFileIdFallbackGlobProbe(path)) {
		return result;
	}
	auto uri = ParseOrThrow(path);
	if (uri.kind == SharepointUriKind::FILE_ID) {
		// A bare id addresses exactly one item; DuckDB globs it before opening.
		result.emplace_back(path);
		return result;
	}
	auto &context = RequireContext(opener, "glob");
	auto auth = GetAuthContext(context, path);

	// Split into the longest literal parent prefix and a trailing segment. This
	// scaffold globs ONE folder level (a wildcard in the final segment);
	// recursive `**` across folders is a documented follow-up (docs/hld.md).
	auto &segs = uri.segments;
	if (segs.empty()) {
		return result;
	}
	const string &last = segs.back();
	string parent;
	for (size_t i = 0; i + 1 < segs.size(); i++) {
		if (i) {
			parent += '/';
		}
		parent += segs[i];
	}
	if (!HasGlobMeta(last)) {
		// A literal path: return it iff it exists.
		GraphClient client(auth);
		auto meta = client.StatByPath(uri.RootRelativePath());
		if (meta.found) {
			result.emplace_back(path);
		}
		return result;
	}
	auto children = GraphClient(auth).ListChildren(parent);
	for (auto &c : children) {
		if (c.is_folder) {
			continue;
		}
		if (MatchGlob(last, c.name)) {
			string full = string(SHAREPOINT_SCHEME) + (parent.empty() ? c.name : parent + "/" + c.name);
			result.emplace_back(full);
		}
	}
	return result;
}

void SharepointFileSystem::FileSync(FileHandle &handle) {
	// Nothing to flush before Close(): the upload happens on Close(). Present
	// so write_blob's handle->Sync() is a no-op rather than unsupported.
}

void SharepointFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.Cast<SharepointFileHandle>().position = location;
}

idx_t SharepointFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<SharepointFileHandle>().position;
}

void SharepointFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw IOException("sharepoint: cannot truncate in place -- Graph replaces whole files");
}

bool SharepointFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	return false; // no hole-punching on Graph
}

} // namespace sharepoint
} // namespace duckdb
