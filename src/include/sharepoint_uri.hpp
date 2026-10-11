#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// sharepoint:// URI parsing. PURE: no duckdb.hpp, no I/O, so the Catch2 binary
// compiles it directly and there is nothing to mock.
//
// Unlike Google Drive, Microsoft Graph addresses driveItems BY PATH natively:
//
//   sharepoint://Finance/2026/actuals.parquet
//       Human-writable path, relative to the root of the drive bound in the
//       secret. Resolves in ONE call:
//         GET /drives/{drive-id}/root:/Finance/2026/actuals.parquet:/content
//       -- there is no per-segment files.list walk, which is why this
//       extension needs none of gdrive's path->id cache machinery.
//
//   sharepoint://id:01ABCDEF...
//       Direct driveItem id. Resolves via /drives/{drive-id}/items/{id}.
//       Prefer this in generated or stored queries -- it is stable across a
//       rename that a path form is not.
//
// WHICH drive (and which SharePoint site) a path hangs off is a property of
// the secret (DRIVE_ID / SITE_ID), never of the URI -- so parsing stays free
// of any notion of "which drive", exactly as gdrive keeps DRIVE_ID in the
// secret. This keeps a corpus portable: the same sharepoint:// paths address
// a different tenant purely by swapping the secret.
// ---------------------------------------------------------------------------

constexpr const char *SHAREPOINT_SCHEME = "sharepoint://";

enum class SharepointUriKind {
	//! sharepoint://a/b/c.parquet -- addressed by path (one Graph call)
	PATH,
	//! sharepoint://id:<itemId> -- already an id
	FILE_ID,
};

struct SharepointUri {
	SharepointUriKind kind = SharepointUriKind::PATH;
	//! Populated when kind == FILE_ID.
	std::string item_id;
	//! Populated when kind == PATH. Empty vector means the drive root.
	std::vector<std::string> segments;

	//! Round-trips back to the canonical textual form. Used in error messages,
	//! so a user is always shown a path they could paste back.
	std::string ToString() const;
	//! The parent path (all but the last segment). Meaningless for FILE_ID.
	std::string ParentPath() const;
	//! The final segment -- the file name. Empty for the root.
	std::string FileName() const;
	//! The path portion after the drive root, e.g. "Finance/2026/x.parquet",
	//! ready to splice into `/drives/{id}/root:/<here>:/content`. Empty for
	//! the root. Meaningless for FILE_ID.
	std::string RootRelativePath() const;
};

struct SharepointUriParse {
	bool ok = false;
	//! Human-readable reason, suitable for putting straight into an exception
	//! message. Empty when ok.
	std::string error;
	SharepointUri uri;
};

//! Cheap prefix test used by FileSystem::CanHandleFile. Must not allocate or
//! validate -- DuckDB calls it on every path it sees, for every filesystem.
bool IsSharepointUri(const std::string &uri);

//! Full parse. Never throws: pure sources cannot depend on DuckDB's exception
//! types, so the DuckDB-side caller turns `error` into an exception.
//!
//! Rules:
//!   - scheme must be exactly "sharepoint://"
//!   - empty and "." and ".." segments are rejected
//!   - repeated slashes collapse; a trailing slash is accepted and ignored
//!   - "id:" with an empty id is an error, and the id: form may carry no
//!     further path segments
//!   - percent-decoding is NOT performed: DuckDB hands us the path un-escaped
SharepointUriParse ParseSharepointUri(const std::string &uri);

//! True iff `uri` has EXACTLY the shape "sharepoint://id:<itemId>/**/*.<ext>"
//! -- the precise string DuckDB core's FALLBACK_GLOB directory-probe
//! heuristic synthesizes when a literal path resolves to zero matches
//! (FileSystem::GlobFileList retries as JoinPath(JoinPath(pattern,"**"),
//! "*."+ext)). An id: form can never be a directory, so the honest answer for
//! this synthesized pattern is "no matches" -- Glob() must return empty rather
//! than surface the deliberate "id: form cannot carry path segments"
//! rejection for a pattern the user never wrote. A real typo like
//! "sharepoint://id:abc/oops" is NOT this shape and keeps surfacing the
//! rejection. (Mirrors the M-5 finding in the sibling gdrive extension.)
bool IsFileIdFallbackGlobProbe(const std::string &uri);

//! True if `path` lies at or under one of the comma-separated prefixes in
//! `csv_prefixes`. Matching is on whole path SEGMENTS, never characters:
//! `sharepoint://lake` matches `sharepoint://lake/data/x` but NOT
//! `sharepoint://lakehouse/x`. Used by `sharepoint_immutable_prefixes`.
bool PathMatchesAnyPrefix(const std::string &path, const std::string &csv_prefixes);

//! True for the drive root itself (`sharepoint://`, `sharepoint:///`): a
//! recursive Graph DELETE of it would empty the whole library, so every
//! delete path refuses it.
bool IsDriveRoot(const SharepointUri &uri);

//! The first character SharePoint forbids in an item name (" * : < > ? \ |),
//! or '\0' if `name` (one path segment) has none. Checked on every path we
//! CREATE, so the user gets "':' is not allowed" instead of Graph's 400 --
//! which for ':' (its own path delimiter) reads "Resource not found for the
//! segment 'root:'". Reads are not checked: such a name cannot exist.
char ForbiddenNameChar(const std::string &name);

} // namespace sharepoint
} // namespace duckdb
