#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

class FileOpener;

namespace sharepoint {

// ---------------------------------------------------------------------------
// Where a bearer token comes from. A file handle can outlive the token it was
// opened with (a long scan, an ATTACHed database kept open for hours), so
// handles hold a SOURCE, not a token: every request asks it for a token that
// is valid now, and it re-mints shortly before expiry.
// ---------------------------------------------------------------------------
class TokenSource {
public:
	virtual ~TokenSource() = default;
	//! A token valid for at least the next minute (served from the shared
	//! token cache; minted when absent or about to expire).
	virtual string Token() = 0;
	//! Discard the cached token and mint a new one -- after a 401, when the
	//! token was revoked or expired early. Returns the new token.
	virtual string Refresh() = 0;
};

// ---------------------------------------------------------------------------
// Everything a Graph call needs to identify itself and locate its data,
// resolved from the active `sharepoint` secret. Cheap to copy: built per
// open, holds the token source and the drive it addresses.
// ---------------------------------------------------------------------------
struct AuthContext {
	//! Supplies `Authorization: Bearer <token>` for each request.
	shared_ptr<TokenSource> tokens;
	//! The Graph drive id (`b!...`) every path hangs off. Resolved from the
	//! secret's DRIVE_ID, or derived from SITE_ID (the site's default document
	//! library) when only a site was given.
	string drive_id;
	//! Graph base URL (sharepoint_graph_endpoint), no trailing slash.
	string graph_endpoint;

	//! Identity fingerprint: SHA-256 over the credential material, the Graph
	//! cloud and the drive -- the key any per-identity cache (block cache) must
	//! include so one principal never sees another's bytes. Never the secret's
	//! name: a recreated or per-connection secret can reuse a name with
	//! different credentials.
	string identity;
};

//! Resolve through the FileOpener, not a ClientContext: DuckDB opens an
//! ATTACHed database file with a database-level opener that has no client
//! connection, and secrets and settings are still reachable through it.
//!
//! Resolve the `sharepoint` secret in scope for `path`, obtaining (and
//! caching until it nears expiry) a bearer token. Throws a DuckDB exception
//! with a fix-teaching message when no secret matches, credentials are
//! rejected, or the drive cannot be resolved.
//!
//! Token acquisition:
//!   * PROVIDER client_credentials -> POST {login}/{tenant}/oauth2/v2.0/token
//!     with grant_type=client_credentials & scope={graph_scope}.
//!   * PROVIDER config with ACCESS_TOKEN -> used as-is (never refreshed).
//!   * PROVIDER config with REFRESH_TOKEN -> grant_type=refresh_token.
AuthContext GetAuthContext(optional_ptr<FileOpener> opener, const string &path);

//! True if any `sharepoint` secret exists at all (whatever its scope). Lets
//! FileExists answer core's speculative probes with false when the extension
//! is simply unconfigured, while user-facing calls still get the loud
//! "no secret configured" error from GetAuthContext.
bool HasAnySharepointSecret(optional_ptr<FileOpener> opener);

} // namespace sharepoint
} // namespace duckdb
