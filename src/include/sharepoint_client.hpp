#pragma once

#include "sharepoint_auth.hpp"

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

#include <cstdint>

namespace duckdb {
namespace sharepoint {

//! Metadata for one driveItem. `ctag` changes on every CONTENT change (Graph's
//! cTag), so it is the right cache-invalidation tag -- `etag` also changes on
//! metadata-only edits and would invalidate too eagerly.
struct DriveItemMeta {
	string id;
	string name;
	int64_t size = 0;
	bool is_folder = false;
	string ctag;
	string etag;
	//! RFC 3339, e.g. "2026-10-10T07:12:03Z" (Graph's lastModifiedDateTime).
	string last_modified;
	bool found = false; // false => the item did not exist (a NOT_FOUND, not a throw)
};

//! One entry from a folder listing.
struct ChildEntry {
	string name;
	bool is_folder = false;
};

// ---------------------------------------------------------------------------
// A thin Microsoft Graph client. One instance per call is fine -- it holds no
// state beyond the AuthContext handed to each method. Every method throws a
// classified DuckDB exception on a non-2xx that is not an expected NOT_FOUND;
// NOT_FOUND is reported in-band (`found=false` / `false` / empty) because it
// is a normal answer on a remote store.
//
// Path forms map to Graph addressing:
//   root-relative path  ->  /drives/{drive}/root:/{path}
//   item id             ->  /drives/{drive}/items/{id}
// ---------------------------------------------------------------------------
//! Sleep before retry number `failed_attempt` per the shared policy
//! (BackoffDelayMs: 500 ms doubling to 20 s, +/-25% per-thread jitter,
//! Retry-After as a floor). Used by every retry loop that talks to Microsoft.
void SleepBeforeRetry(int failed_attempt, int retry_after_seconds);

class GraphClient {
public:
	explicit GraphClient(const AuthContext &auth) : auth(auth) {
	}

	//! Metadata by root-relative path (empty path == the drive root).
	DriveItemMeta StatByPath(const string &root_relative_path);
	//! Metadata by item id.
	DriveItemMeta StatById(const string &item_id);

	//! Read [offset, offset+length) of an item's content into `out`. Sends a
	//! Range header; Graph 302-redirects /content to a pre-authenticated
	//! download URL, which is fetched verbatim by PreauthenticatedRequest
	//! (never by httplib, which would re-sort its signed query -> 401).
	void ReadRange(const string &item_id, int64_t offset, int64_t length, string &out);

	//! Upload the whole of `bytes` to a root-relative path, replacing any
	//! existing file. Chooses a single PUT for small files and a resumable
	//! upload session for large ones (threshold/chunk from
	//! sharepoint_upload_chunk_bytes). Returns the new item's metadata.
	DriveItemMeta PutContent(const string &root_relative_path, const string &bytes, idx_t chunk_bytes);

	//! DELETE an item (recycle bin unless `permanent`). Returns false if it
	//! was already absent.
	bool DeleteItem(const string &item_id, bool permanent);

	//! PATCH an item's parent/name to move or rename it. Both paths are
	//! root-relative and on this same drive.
	void MoveByPath(const string &source_root_relative, const string &target_root_relative);

	//! Create a folder at a root-relative path (idempotent: an existing folder
	//! is not an error; a file of that name is).
	void CreateFolder(const string &root_relative_path);

	//! List the immediate children of a folder given by root-relative path,
	//! following @odata.nextLink pages.
	vector<ChildEntry> ListChildren(const string &folder_root_relative_path, bool *found = nullptr);

private:
	const AuthContext &auth;
};

} // namespace sharepoint
} // namespace duckdb
