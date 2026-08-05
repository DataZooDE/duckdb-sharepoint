#pragma once

#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Microsoft Graph error classification. PURE: maps an HTTP status (and the
// optional `error.code` string Graph returns in its JSON body) to a stable
// kind, so the DuckDB-side caller can turn it into the right DuckDB exception
// type WITHOUT matching on human-readable message text (which Graph localises
// and reworders across API versions).
// ---------------------------------------------------------------------------

enum class GraphErrorKind {
	//! 2xx -- not an error. Present so a classifier has a total mapping.
	OK,
	//! 401 / invalidAuthenticationToken / expired token. Caller should refresh
	//! once and retry; a second occurrence is fatal.
	UNAUTHENTICATED,
	//! 403 -- authenticated but not permitted (missing Graph application
	//! permission, or the app has no access to this site/drive).
	FORBIDDEN,
	//! 404 / itemNotFound. The one classification callers turn into "absent"
	//! rather than an exception on remove_file / file_size.
	NOT_FOUND,
	//! 409 -- nameAlreadyExists and friends.
	CONFLICT,
	//! 429 or 503 with Retry-After -- throttled. Retryable after the delay.
	THROTTLED,
	//! 5xx other than a throttling 503.
	SERVER,
	//! Anything else, including a transport failure with no HTTP status.
	OTHER,
};

//! Classify from the HTTP status code and Graph's `error.code` (may be empty).
//! `error.code` refines a status where the status alone is ambiguous (e.g.
//! a 400 carrying `activityLimitReached` is really throttling).
GraphErrorKind ClassifyGraphError(int http_status, const std::string &graph_error_code);

//! True for kinds a caller may retry (after any Retry-After delay): THROTTLED
//! and SERVER. UNAUTHENTICATED is retryable exactly once after a token
//! refresh, which is a different path, so it is NOT included here.
bool IsRetryable(GraphErrorKind kind);

//! A short, stable, credential-free label for a kind -- used to build
//! exception messages on the DuckDB side.
const char *GraphErrorKindName(GraphErrorKind kind);

} // namespace sharepoint
} // namespace duckdb
