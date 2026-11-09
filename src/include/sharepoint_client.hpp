#pragma once

#include "sharepoint_auth.hpp"
#include "sharepoint_upload.hpp"

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

//! The path+query of an absolute @odata.nextLink, for the Graph connection
//! (bound to one host; Graph keeps the host across pages).
string GraphPathOfLink(const string &link);

//! Percent-encode a root-relative path for Graph's `root:/{path}` form ('/'
//! stays the separator).
string EncodeGraphPath(const string &path);

//! Percent-encode one path segment of a workbook resource (a table or
//! worksheet name, a range address): everything but unreserved characters,
//! '/' included.
string EncodeWorkbookName(const string &name);

//! One open upload session. `upload_url` is a credential (pre-authenticated)
//! and is empty once the session is committed or aborted.
struct GraphUploadSession {
	string root_relative_path;
	string upload_url;
	//! The item before the session opened: tells a lost commit reply apart
	//! from a failed commit.
	DriveItemMeta before;
	bool IsOpen() const {
		return !upload_url.empty();
	}
};

class GraphClient {
public:
	explicit GraphClient(const AuthContext &auth) : auth(auth) {
	}

	//! Metadata by root-relative path (empty path == the drive root).
	DriveItemMeta StatByPath(const string &root_relative_path);
	//! Metadata by item id.
	DriveItemMeta StatById(const string &item_id);
	//! Metadata of an earlier version ("3.0") of item `item_id` (#66): its
	//! size and time; `id` stays the item's, `ctag` is "version:<v>" (a
	//! version never changes, so blocks cached under it never go stale).
	DriveItemMeta StatVersion(const string &item_id, const string &version);

	//! Read [offset, offset+length) of an item's content into `out`. Sends a
	//! Range header; Graph 302-redirects /content to a pre-authenticated
	//! download URL, which is fetched verbatim by PreauthenticatedRequest
	//! (never by httplib, which would re-sort its signed query -> 401).
	//! With `version`, the bytes of that earlier version (/versions/<v>/content).
	void ReadRange(const string &item_id, int64_t offset, int64_t length, string &out, const string &version = "");

	//! Upload `bytes` -- at most one upload chunk -- to a root-relative path
	//! in a single PUT, replacing any existing file. Returns the new item's
	//! metadata. Anything larger streams through an upload session instead.
	DriveItemMeta PutContent(const string &root_relative_path, const string &bytes);

	//! Streaming upload, driven by a write handle (sharepoint_filesystem.cpp):
	//! Open a deferCommit upload session; Send each full chunk as it fills
	//! (and the last one, `final`, carrying the total); Commit to publish the
	//! file atomically. Abort discards the session -- nothing is published --
	//! and never throws. Each chunk is retried with the shared backoff and
	//! resumed from the session's nextExpectedRanges.
	void OpenUploadSession(const string &root_relative_path, GraphUploadSession &session);
	void SendUploadChunk(GraphUploadSession &session, const ChunkedUploadBuffer &buf, bool final);
	DriveItemMeta CommitUploadSession(GraphUploadSession &session, uint64_t total);
	void AbortUploadSession(GraphUploadSession &session) noexcept;

	//! DELETE an item (recycle bin unless `permanent`). Returns false if it
	//! was already absent.
	bool DeleteItem(const string &item_id, bool permanent);

	//! PATCH an item's parent/name to move or rename it. Both paths are
	//! root-relative and on this same drive.
	void MoveByPath(const string &source_root_relative, const string &target_root_relative);
	//! The same move for an item whose id is known (one this process just
	//! wrote); `source_root_relative` names it in errors.
	void MoveById(const string &item_id, const string &source_root_relative, const string &target_root_relative);

	//! Create a folder at a root-relative path (idempotent: an existing folder
	//! is not an error; a file of that name is).
	void CreateFolder(const string &root_relative_path);

	//! List the immediate children of a folder given by root-relative path,
	//! following @odata.nextLink pages.
	vector<ChildEntry> ListChildren(const string &folder_root_relative_path, bool *found = nullptr);

	//! GET a workbook resource of an .xlsx item and return its JSON body:
	//! /drives/{drive}/items/{item_id}/workbook/{resource}. `resource` is sent
	//! as given, so the caller percent-encodes names in it (EncodeWorkbookName).
	//! Sessionless: each call opens the saved workbook. A 404 throws an
	//! InvalidInputException naming `what` ("table 'Sales'"), since there it
	//! means the user named something that is not in the workbook.
	string WorkbookGet(const string &item_id, const string &resource, const string &what);

	//! Any workbook request: `method` GET, POST or PATCH, `json_body` sent as
	//! application/json, `session_id` as the workbook-session-id header when
	//! not empty. `idempotent = false` for a request that must not be applied
	//! twice (rows/add, tables/add): it is re-sent only after throttling.
	//! With `status` given, a 404 is returned in-band (empty body) instead of
	//! thrown; every other non-2xx throws.
	string WorkbookCall(const string &method, const string &item_id, const string &resource, const string &json_body,
	                    const string &session_id, const string &what, bool idempotent, int *status = nullptr);

	//! GET any Graph path ("/v1.0/sites/..."; a nextLink via GraphPathOfLink)
	//! and return its JSON body. With `status` given, a 404 is returned
	//! in-band (empty body); every other non-2xx throws, naming `what`.
	//! `prefer`, when not empty, is sent as the Prefer header (e.g. Outlook's
	//! outlook.body-content-type="text").
	string GraphGet(const string &path, const string &what, int *status = nullptr, const string &prefer = string());

	//! POST or PATCH a body (JSON unless `content_type` says otherwise, e.g.
	//! a OneNote page's text/html) to any Graph path and return the reply's
	//! body; non-2xx throws, naming `what`. `idempotent = false` (a $batch
	//! that creates items): re-sent only after throttling.
	string GraphSend(const string &method, const string &path, const string &json_body, const string &what,
	                 bool idempotent, const string &content_type = "application/json");

private:
	const AuthContext &auth;
};

//! GET `path` from SharePoint's own REST API on https://<host> (#97: list
//! attachments, which Graph has no API for), with a token from `tokens` and
//! the retries Graph calls get. The status; `body` gets the answer whatever
//! it is. Throws only when no answer came. `host` must be a SharePoint host
//! (GetSharepointRestTokens checks it).
int SharepointRestGet(TokenSource &tokens, const string &host, const string &path, string &body);

} // namespace sharepoint
} // namespace duckdb
