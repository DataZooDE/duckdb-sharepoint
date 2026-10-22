#pragma once

#include "sharepoint_blockcache.hpp"
#include "sharepoint_client.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <cstdint>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// SharepointFileSystem -- the extension point.
//
// DuckDB routes every file access through a virtual filesystem, so read_parquet,
// read_csv, COPY, glob and a DuckLake DATA_PATH all inherit sharepoint:// from
// here. Because Microsoft Graph addresses items by path, this filesystem is a
// good deal simpler than the sibling gdrive one: there is no path->id cache
// (a path resolves in one call) and no duplicate-sibling-name ambiguity
// (SharePoint forbids it).
//
// Two mappings are deliberately hostile, matching gdrive:
//   * Write at a byte offset throws. Graph replaces whole files; it cannot
//     write at an offset, and failing loudly beats corrupting silently.
//   * MoveFile is not atomic. DuckDB's storage manager assumes rename
//     atomicity; Graph's PATCH does not guarantee it. Documented, not papered
//     over.
// ---------------------------------------------------------------------------

class SharepointFileHandle : public FileHandle {
public:
	SharepointFileHandle(FileSystem &fs, string path, FileOpenFlags flags, AuthContext auth);
	//! Aborts an upload session left open (a handle destroyed without a
	//! successful Close(), e.g. a COPY that failed mid-stream): nothing is
	//! published. Best effort, never throws.
	~SharepointFileHandle() override;

	void Close() override;
	//! Discard an in-flight write WITHOUT committing it: the upload session
	//! is discarded and a later Close() is a no-op. See
	//! SharepointFileSystem::AbortFileWrite.
	void CancelWrite() noexcept;

	AuthContext auth;
	//! Read path: resolved once at OpenFile and reused.
	DriveItemMeta meta;
	//! Sequential read/write cursor.
	idx_t position = 0;
	//! Block size for cached reads, captured at OpenFile from
	//! sharepoint_block_size_bytes. 0 disables the cache (exact ranges).
	idx_t block_size_bytes = 8ULL * 1024 * 1024;

	//! Write path: bytes stream through `upload`, which holds at most ONE
	//! chunk (sharepoint_upload_chunk_bytes, captured at OpenFile). A file
	//! that never outgrows it is sent as a single PUT on Close(); the moment
	//! it does, a deferCommit upload session is opened and every full chunk
	//! goes up as it fills. The file is published only by the commit in
	//! Close(), so an error mid-stream leaves nothing behind -- one exception:
	//! while a NEW file's session is open, SharePoint shows a 0-byte
	//! placeholder at its path (removed when the session is discarded; COPY's
	//! tmp_ staging keeps it off the real name). `write_failed`
	//! stops Close() publishing a truncated file after an error unwound
	//! mid-write.
	bool is_write = false;
	string write_root_relative;
	bool write_failed = false;
	unique_ptr<ChunkedUploadBuffer> upload;
	GraphUploadSession session;
};

class SharepointFileSystem : public FileSystem {
public:
	SharepointFileSystem() = default;
	~SharepointFileSystem() override = default;

	bool CanHandleFile(const string &fpath) override;
	//! sharepoint:// paths are already canonical. The base implementation
	//! collapses "//" for any scheme missing from DuckDB's hard-coded list of
	//! remote prefixes, which turned `ATTACH 'sharepoint://a/db.duckdb'` into
	//! "sharepoint:/a/db.duckdb" and "database does not exist". (DuckDB 1.5+.)
	string CanonicalizePath(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		return path;
	}
	std::string GetName() const override {
		return "SharepointFileSystem";
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	//! DuckDB 2.0 calls this when a COPY fails mid-write. The base default is
	//! handle.Close() -- which here COMMITS the truncated file, and over an
	//! existing file leaves it there (2.0's cleanup preserves pre-existing
	//! targets). Discard instead. Deliberately NOT `override`: DuckDB 1.x has
	//! no such hook, so there it is an uncalled method; on 2.0 it overrides.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#endif
	void AbortFileWrite(FileHandle &handle); // NOLINT(modernize-use-override)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

	int64_t GetFileSize(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
	void MoveFile(const string &source, const string &target,
	              optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	void FileSync(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}

	//! Forget cached metadata for "<identity>|<root-relative path>" and everything
	//! beneath it (a folder). Called after every write, move and delete we make.
	void InvalidateMeta(const string &key);
	//! Same, for every entry describing item `item_id` (a delete by id: form
	//! does not know the path it was cached under).
	void InvalidateMetaById(const string &item_id);

	//! Graph cannot write at an offset or trim in place; both throw.
	void Truncate(FileHandle &handle, int64_t new_size) override;
	bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;

private:
	//! Process-wide read cache: this FileSystem is one object for the whole
	//! DatabaseInstance, so the cache spans every ClientContext. Keys carry the
	//! caller identity + cTag so it is never a cross-tenant or cross-version leak.
	SharepointBlockCache block_cache;

	//! Metadata of files under sharepoint_immutable_prefixes, keyed
	//! "<identity>|<root-relative path>": the user promised those files are
	//! never rewritten in place, so a remembered stat cannot be stale and the
	//! per-open Graph round trip is skipped (gdrive_immutable_prefixes does the
	//! same). Our own writes, moves and deletes invalidate entries; anything
	//! done to those files by OTHER clients is the user's promise to keep.
	mutex meta_cache_lock;
	unordered_map<string, DriveItemMeta> meta_cache;
	bool MetaCacheGet(const string &key, DriveItemMeta &out);
	void MetaCachePut(const string &key, const DriveItemMeta &meta);
};

} // namespace sharepoint
} // namespace duckdb
