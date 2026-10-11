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
	//! 400 that is not throttling -- the request itself is wrong (e.g. a name
	//! SharePoint forbids). Never retried.
	BAD_REQUEST,
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

//! Which DuckDB exception a kind becomes (the DuckDB side maps these to
//! IOException / PermissionException / InvalidInputException), so callers can
//! tell "not allowed" and "bad request" from transient I/O.
enum class GraphExceptionType { IO, PERMISSION, INVALID_INPUT };
GraphExceptionType ExceptionTypeFor(GraphErrorKind kind);

//! One-line exception text: "sharepoint: <op> failed: HTTP <status> (<kind>[,
//! <graph code>])[: <graph message>]". Graph's message is single-lined and
//! truncated; it never carries credentials (Graph does not echo tokens).
std::string FormatGraphError(const std::string &op, int http_status, const std::string &graph_error_code,
                             const std::string &graph_message);

// ---------------------------------------------------------------------------
// Retry policy, as in the sibling gdrive: up to kMaxGraphAttempts attempts per
// call, delay doubling from 500 ms and capped at 20 s, +/-25% jitter so
// concurrent scan threads do not retry in lockstep, and the server's
// Retry-After as a floor. Pure: the caller draws the jitter and sleeps.
// ---------------------------------------------------------------------------
constexpr int kMaxGraphAttempts = 5;
//! Retry-After values above this are clamped: a hostile or broken header must
//! not park a DuckDB thread for hours.
constexpr int kMaxRetryAfterSeconds = 120;

//! Retry-After in its delay-seconds form; 0 for absent, an HTTP-date or junk.
int ParseRetryAfterSeconds(const std::string &header);

//! Delay before the attempt after `failed_attempt` (1-based). `jitter` is
//! clamped to [0.75, 1.25]; the result is never below Retry-After.
long long BackoffDelayMs(int failed_attempt, int retry_after_seconds, double jitter);

} // namespace sharepoint
} // namespace duckdb
