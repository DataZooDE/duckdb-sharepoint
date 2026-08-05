#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

class ClientContext;

namespace sharepoint {

// ---------------------------------------------------------------------------
// Everything a Graph call needs to identify itself and locate its data,
// resolved from the active `sharepoint` secret. Cheap value type: built per
// query, holds the bearer token to send and the drive it addresses.
// ---------------------------------------------------------------------------
struct AuthContext {
	//! Bearer token to send as `Authorization: Bearer <access_token>`.
	string access_token;
	//! The Graph drive id (`b!...`) every path hangs off. Resolved from the
	//! secret's DRIVE_ID, or derived from SITE_ID (the site's default document
	//! library) when only a site was given.
	string drive_id;
	//! Graph base URL (sharepoint_graph_endpoint), no trailing slash.
	string graph_endpoint;

	//! Identity fingerprint (secret name + drive) -- the key any per-identity
	//! cache (token, future block cache) must include so one tenant never sees
	//! another's bytes. Mirrors the CacheKey security note in the sibling
	//! gdrive extension.
	string identity;
};

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
AuthContext GetAuthContext(ClientContext &context, const string &path);

//! Drop any cached token for a secret -- called when a secret is dropped or
//! recreated so a stale token is never reused.
void InvalidateAuthCache(const string &secret_name);

} // namespace sharepoint
} // namespace duckdb
