#pragma once

#include "sharepoint_client.hpp"

#include "duckdb/common/file_system.hpp"

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
	~SharepointFileHandle() override = default;

	void Close() override;

	AuthContext auth;
	//! Read path: resolved once at OpenFile and reused.
	DriveItemMeta meta;
	//! Sequential read/write cursor.
	idx_t position = 0;

	//! Write path: bytes accumulate locally and nothing reaches Graph until
	//! Close() (Graph has no append/positional write). `write_root_relative`
	//! is where they go; `write_failed` stops Close() uploading a truncated
	//! file after an error unwound mid-write.
	bool is_write = false;
	string write_buffer;
	string write_root_relative;
	bool write_failed = false;
	//! Captured at OpenFile (where the setting is reachable): files at or below
	//! this go up as one PUT, larger ones via a resumable upload session.
	idx_t upload_chunk_bytes = 10ULL * 1024 * 1024;
};

class SharepointFileSystem : public FileSystem {
public:
	SharepointFileSystem() = default;
	~SharepointFileSystem() override = default;

	bool CanHandleFile(const string &fpath) override;
	std::string GetName() const override {
		return "SharepointFileSystem";
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	int64_t GetFileSize(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;

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

	//! Graph cannot write at an offset or trim in place; both throw.
	void Truncate(FileHandle &handle, int64_t new_size) override;
	bool Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override;
};

} // namespace sharepoint
} // namespace duckdb
