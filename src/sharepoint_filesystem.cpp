// The sharepoint:// virtual filesystem.
//
// STATUS: network frontier of the scaffold (see sharepoint_auth.cpp's header).
// The DuckDB FileSystem method surface is complete and the Graph mapping is
// implemented, but nothing here has run against a live tenant yet.
#include "sharepoint_filesystem.hpp"
#include "sharepoint_banner.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_counters.hpp"
#include "sharepoint_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_set>

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

idx_t BlockSizeBytes(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("sharepoint_block_size_bytes", v) && !v.IsNull()) {
		// Capped: each block is one ranged GET held in memory, so an unbounded
		// value would turn every small read into a whole-file download.
		return MinValue<idx_t>(v.GetValue<idx_t>(), 64ULL * 1024 * 1024);
	}
	return 8ULL * 1024 * 1024;
}

string ImmutablePrefixes(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("sharepoint_immutable_prefixes", v) && !v.IsNull()) {
		return v.ToString();
	}
	return string();
}

//! Entries the immutable-prefix metadata cache may hold before it is simply
//! cleared: a few hundred bytes each, so ~5 MB worst case.
constexpr idx_t kMaxMetaCacheEntries = 20000;

//! Refuse to CREATE an item whose name SharePoint forbids -- before Graph
//! answers with an opaque 400.
void CheckCreatableNames(const SharepointUri &uri, const string &path) {
	for (const auto &segment : uri.segments) {
		const char bad = ForbiddenNameChar(segment);
		if (bad != '\0') {
			throw InvalidInputException("sharepoint: '%s': SharePoint does not allow '%s' in file or folder names "
			                            "(forbidden: \" * : < > ? \\ |)",
			                            path, string(1, bad));
		}
	}
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
	is_write = false;
	GraphClient client(auth);
	client.PutContent(write_root_relative, write_buffer, upload_chunk_bytes);
	static_cast<SharepointFileSystem &>(file_system).InvalidateMeta(auth.identity + "|" + write_root_relative);
}

bool SharepointFileSystem::MetaCacheGet(const string &key, DriveItemMeta &out) {
	lock_guard<mutex> guard(meta_cache_lock);
	auto it = meta_cache.find(key);
	if (it == meta_cache.end()) {
		return false;
	}
	out = it->second;
	return true;
}

void SharepointFileSystem::MetaCachePut(const string &key, const DriveItemMeta &meta) {
	lock_guard<mutex> guard(meta_cache_lock);
	if (meta_cache.size() >= kMaxMetaCacheEntries) {
		meta_cache.clear(); // crude but bounded; a miss only costs one stat
	}
	meta_cache[key] = meta;
}

void SharepointFileSystem::InvalidateMeta(const string &key) {
	lock_guard<mutex> guard(meta_cache_lock);
	meta_cache.erase(key);
	const string folder_prefix = key + "/";
	for (auto it = meta_cache.begin(); it != meta_cache.end();) {
		if (it->first.compare(0, folder_prefix.size(), folder_prefix) == 0) {
			it = meta_cache.erase(it);
		} else {
			++it;
		}
	}
}

bool SharepointFileSystem::CanHandleFile(const string &fpath) {
	return IsSharepointUri(fpath);
}

unique_ptr<FileHandle> SharepointFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                      optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto uri = ParseOrThrow(path);
		auto &context = RequireContext(opener, "open");
		auto auth = GetAuthContext(context, path);

		if (flags.OpenForAppending()) {
			// Graph replaces whole files; "append" would silently drop the existing
			// content. Refuse instead.
			throw NotImplementedException("sharepoint: appending to '%s' is not supported -- Graph replaces whole files",
			                              path);
		}
		if (flags.OpenForWriting()) {
			if (uri.kind != SharepointUriKind::PATH) {
				throw IOException("sharepoint: writing addresses a path, not an id: form ('%s')", path);
			}
			CheckCreatableNames(uri, path);
			auto handle = make_uniq<SharepointFileHandle>(*this, path, flags, std::move(auth));
			handle->is_write = true;
			handle->write_root_relative = uri.RootRelativePath();
			handle->upload_chunk_bytes = UploadChunkBytes(context);
			return std::move(handle);
		}

		// Read: resolve metadata once -- or not at all, for a file under a prefix
		// the user declared immutable whose metadata we already hold.
		const string prefixes = uri.kind == SharepointUriKind::PATH ? ImmutablePrefixes(context) : string();
		const bool immutable = !prefixes.empty() && PathMatchesAnyPrefix(path, prefixes);
		const string meta_key = auth.identity + "|" + uri.RootRelativePath();
		DriveItemMeta meta;
		if (immutable && MetaCacheGet(meta_key, meta)) {
			IncrementCounter(Counter::META_CACHE_HIT);
		} else {
			GraphClient client(auth);
			meta = (uri.kind == SharepointUriKind::FILE_ID) ? client.StatById(uri.item_id)
			                                                : client.StatByPath(uri.RootRelativePath());
			if (immutable && meta.found && !meta.is_folder) {
				MetaCachePut(meta_key, meta);
			}
		}
		if (meta.found && meta.is_folder) {
			// A folder is not "absent": returning null here made remove_file report
			// false and file_size NULL for an existing folder.
			throw IOException("sharepoint: '%s' is a directory, not a file", path);
		}
		if (!meta.found) {
			if (flags.ReturnNullIfNotExists()) {
				return nullptr;
			}
			throw IOException("sharepoint: no such file: '%s'", path);
		}
		auto handle = make_uniq<SharepointFileHandle>(*this, path, flags, std::move(auth));
		handle->meta = std::move(meta);
		handle->block_size_bytes = BlockSizeBytes(context);
		return std::move(handle);
	});
}

void SharepointFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = handle.Cast<SharepointFileHandle>();
	if (nr_bytes <= 0) {
		return;
	}

	// Out of bounds: answer locally with the short-read error DuckDB expects,
	// instead of a ranged GET that Graph rejects with 416. Written so that
	// location + nr_bytes cannot wrap.
	const idx_t size = static_cast<idx_t>(h.meta.size);
	if (location > size || static_cast<idx_t>(nr_bytes) > size - location) {
		throw IOException("sharepoint: short read of '%s' (wanted %lld bytes at %llu, file has %llu)",
		                  handle.path.c_str(), (long long)nr_bytes, (unsigned long long)location,
		                  (unsigned long long)size);
	}

	// Cache disabled (block size 0), or no cTag to version the blocks by (a
	// rewrite would then hit stale blocks): go straight to a ranged GET.
	if (h.block_size_bytes == 0 || h.meta.ctag.empty()) {
		GraphClient client(h.auth);
		string chunk;
		client.ReadRange(h.meta.id, static_cast<int64_t>(location), nr_bytes, chunk);
		if (static_cast<int64_t>(chunk.size()) < nr_bytes) {
			throw IOException("sharepoint: short read of '%s' (wanted %lld, got %llu)", handle.path.c_str(),
			                  (long long)nr_bytes, (unsigned long long)chunk.size());
		}
		std::memcpy(buffer, chunk.data(), nr_bytes);
		return;
	}

	// Cached path: block key carries identity + item + cTag so it is never a
	// cross-tenant or stale-version hit (the cache adds the block size).
	// Missing blocks come from a ranged GET.
	const string key = h.auth.identity + "|" + h.meta.id + "|" + h.meta.ctag;
	const AuthContext auth = h.auth;
	const string item = h.meta.id;
	block_cache.Read(
	    key, static_cast<uint64_t>(h.block_size_bytes), static_cast<uint64_t>(h.meta.size),
	    static_cast<uint64_t>(location), static_cast<uint64_t>(nr_bytes), static_cast<char *>(buffer),
	    [&auth, &item](uint64_t block_offset, uint64_t block_length) {
		    string chunk;
		    GraphClient(auth).ReadRange(item, static_cast<int64_t>(block_offset),
		                                static_cast<int64_t>(block_length), chunk);
		    if (chunk.size() < block_length) {
			    throw IOException("sharepoint: short read of block at %llu (wanted %llu, got %llu)",
			                      (unsigned long long)block_offset, (unsigned long long)block_length,
			                      (unsigned long long)chunk.size());
		    }
		    return chunk;
	    });
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
	try {
		h.write_buffer.append(static_cast<const char *>(buffer), static_cast<size_t>(nr_bytes));
	} catch (...) {
		// Never publish what we have so far as if it were the whole file: Close()
		// skips the upload once a write failed (gdrive does the same).
		h.write_failed = true;
		throw;
	}
	h.position += nr_bytes;
	return nr_bytes;
}

int64_t SharepointFileSystem::GetFileSize(FileHandle &handle) {
	auto &h = handle.Cast<SharepointFileHandle>();
	return h.is_write ? static_cast<int64_t>(h.write_buffer.size()) : h.meta.size;
}

namespace {

//! RFC 3339 (Graph's lastModifiedDateTime) -> DuckDB timestamp, as in the
//! sibling gdrive. Graph sends a 'T' separator and a 'Z'; Timestamp::FromString
//! wants a space and an explicit offset. Unparseable -> epoch rather than a
//! failed scan: the time is metadata, never data.
timestamp_t ParseRfc3339(const string &rfc3339) {
	if (rfc3339.empty()) {
		return timestamp_t(0);
	}
	string normalised = rfc3339;
	auto t_pos = normalised.find('T');
	if (t_pos != string::npos) {
		normalised[t_pos] = ' ';
	}
	if (normalised.back() == 'Z') {
		normalised.pop_back();
		normalised += "+00";
	}
	try {
		return Timestamp::FromString(normalised, true);
	} catch (...) {
		return timestamp_t(0);
	}
}

} // namespace

timestamp_t SharepointFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return ParseRfc3339(handle.Cast<SharepointFileHandle>().meta.last_modified);
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
	if (!context || !HasAnySharepointSecret(*context)) {
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
	if (!context || !HasAnySharepointSecret(*context)) {
		return false;
	}
	auto parsed = ParseSharepointUri(directory);
	if (!parsed.ok || parsed.uri.kind == SharepointUriKind::FILE_ID) {
		return false;
	}
	if (parsed.uri.RootRelativePath().empty()) {
		return true; // the drive root always exists
	}
	auto auth = GetAuthContext(*context, directory);
	GraphClient client(auth);
	auto meta = client.StatByPath(parsed.uri.RootRelativePath());
	return meta.found && meta.is_folder;
}

void SharepointFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto uri = ParseOrThrow(directory);
		CheckCreatableNames(uri, directory);
		auto &context = RequireContext(opener, "create directory");
		auto auth = GetAuthContext(context, directory);
		GraphClient(auth).CreateFolder(uri.RootRelativePath());
	});
}

void SharepointFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		// A directory is a driveItem too; resolve and delete it (recursively, as
		// Graph deletes a folder and its contents).
		auto uri = ParseOrThrow(directory);
		auto &context = RequireContext(opener, "remove directory");
		if (uri.kind != SharepointUriKind::PATH || uri.RootRelativePath().empty()) {
			// The empty path is the drive root: a recursive delete of the whole
			// library is never what `remove directory` on a URI typo should do.
			throw IOException("sharepoint: refusing to remove '%s' (the drive root, or not a folder path)", directory);
		}
		auto auth = GetAuthContext(context, directory);
		GraphClient client(auth);
		auto meta = client.StatByPath(uri.RootRelativePath());
		if (!meta.found) {
			return;
		}
		if (!meta.is_folder) {
			throw IOException("sharepoint: '%s' is a file, not a directory", directory);
		}
		client.DeleteItem(meta.id, PermanentDelete(context));
		InvalidateMeta(auth.identity + "|" + uri.RootRelativePath());
	});
}

bool SharepointFileSystem::ListFiles(const string &directory,
                                     const std::function<void(const string &, bool)> &callback,
                                     FileOpener *opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto uri = ParseOrThrow(directory);
		auto &context = RequireContext(opener, "list files");
		auto auth = GetAuthContext(context, directory);
		bool found = false;
		auto children = GraphClient(auth).ListChildren(uri.RootRelativePath(), &found);
		for (auto &c : children) {
			callback(c.name, c.is_folder);
		}
		// FileSystem contract: true if the directory exists -- an empty folder too.
		return found;
	});
}

void SharepointFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
		auto src = ParseOrThrow(source);
		auto dst = ParseOrThrow(target);
		if (src.kind != SharepointUriKind::PATH || dst.kind != SharepointUriKind::PATH) {
			throw IOException("sharepoint: move addresses paths on both sides, not id: forms");
		}
		CheckCreatableNames(dst, target);
		auto &context = RequireContext(opener, "move");
		auto auth = GetAuthContext(context, source);
		// The target may fall under a different secret (scoped secrets) and hence a
		// different drive. Graph moves within one drive here; silently moving into
		// the SOURCE's drive instead would put the file somewhere nobody asked for.
		auto target_auth = GetAuthContext(context, target);
		if (target_auth.identity != auth.identity) {
			throw IOException("sharepoint: cannot move '%s' to '%s': they resolve to different secrets or drives",
			                  source, target);
		}
		GraphClient(auth).MoveByPath(src.RootRelativePath(), dst.RootRelativePath());
		InvalidateMeta(auth.identity + "|" + src.RootRelativePath());
		InvalidateMeta(auth.identity + "|" + dst.RootRelativePath());
	});
}

void SharepointFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
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
		if (uri.kind == SharepointUriKind::PATH) {
			InvalidateMeta(auth.identity + "|" + uri.RootRelativePath());
		}
	});
}

vector<OpenFileInfo> SharepointFileSystem::Glob(const string &path, FileOpener *opener) {
	DATAZOO_GUARDED_BLOCK(SHAREPOINT_BANNER, {
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
		GraphClient client(auth);

		// {a,b} alternation first, over the whole root-relative pattern (an
		// alternative may contain '/'); each expansion is then globbed on its own.
		const auto patterns = ExpandBraces(uri.RootRelativePath());
		if (patterns.empty()) {
			throw InvalidInputException("sharepoint: glob pattern '%s' expands to too many alternatives", path);
		}
		std::unordered_set<string> seen;
		auto emit = [&](const string &rel) {
			if (seen.insert(rel).second) {
				result.emplace_back(string(SHAREPOINT_SCHEME) + rel);
			}
		};
		for (const auto &pattern : patterns) {
			auto parsed = ParseSharepointUri(string(SHAREPOINT_SCHEME) + pattern);
			if (!parsed.ok || parsed.uri.kind != SharepointUriKind::PATH || parsed.uri.segments.empty()) {
				continue;
			}
			const auto &segs = parsed.uri.segments;

			// A fully literal path is one Stat, not a tree walk -- and it is the only
			// case where a matching item may be a folder (DuckDB globs a bare path
			// before opening it).
			bool has_meta = false;
			for (auto &seg : segs) {
				if (HasGlobMetacharacters(seg)) {
					has_meta = true;
					break;
				}
			}
			if (!has_meta) {
				if (client.StatByPath(parsed.uri.RootRelativePath()).found) {
					emit(parsed.uri.RootRelativePath());
				}
				continue;
			}

			// Wildcard pattern: walk folders through GraphClient::ListChildren. `**`
			// recurses across levels; `*`/`?`/`[...]` stay within a segment. The
			// matcher is pure (sharepoint_glob.*) and unit-tested; here we only adapt
			// the lister.
			auto lister = [&client](const string &folder) -> vector<GlobEntry> {
				vector<GlobEntry> entries;
				for (auto &c : client.ListChildren(folder)) {
					entries.push_back(GlobEntry {c.name, c.is_folder});
				}
				return entries;
			};
			// Qualify: FileSystem also declares a GlobFiles member, which unqualified
			// would shadow our pure matcher here inside a FileSystem subclass.
			for (auto &rel : sharepoint::GlobFiles(segs, lister)) {
				emit(rel);
			}
		}
		// Names may legitimately contain glob characters ("report[1].csv",
		// "{draft}.txt"). When a pattern matched nothing, try it as a literal path,
		// as DuckDB's local filesystem does -- otherwise such a file could never be
		// read by its own name. Costs one stat, only on an empty match.
		if (result.empty() && HasGlobMetacharacters(uri.RootRelativePath())) {
			auto meta = client.StatByPath(uri.RootRelativePath());
			if (meta.found && !meta.is_folder) {
				emit(uri.RootRelativePath());
			}
		}
		return result;
	});
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
