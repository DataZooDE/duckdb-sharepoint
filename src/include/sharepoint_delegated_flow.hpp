#pragma once

#include "duckdb/common/string.hpp"

#include <cstdint>
#include <functional>

namespace duckdb {

class ClientContext;
struct SecretMatch;

namespace sharepoint {

// ---------------------------------------------------------------------------
// Delegated (user) sign-in -- the I/O half. Request bodies, reply parsing and
// the polling rules are pure (sharepoint_delegated.hpp); this file sends them.
//
// Every call to the Entra ID endpoints here goes through the raw TLS transport
// (sharepoint_preauth.hpp): one deadline over connect + handshake + exchange,
// SNI and certificate-hostname verification armed or the call fails closed,
// and a bounded response size.
// ---------------------------------------------------------------------------

struct DelegatedFlowConfig {
	string secret_name;
	//! "device_code" or "authorization_code" (for messages).
	string provider;
	//! Already checked by IsAllowedLoginEndpoint, no trailing slash.
	string login_endpoint;
	//! Already checked by IsValidTenantId.
	string tenant;
	string client_id;
	string scope;
	//! authorization_code: the localhost redirect port.
	int redirect_port = 8020;
	//! device_code: give up after this long (capped at the code's own life).
	int64_t timeout_seconds = 900;
};

struct DelegatedTokens {
	string access_token;
	string refresh_token;
	int64_t expires_in = 0;
};

//! RFC 8628 device authorization grant. Prints ONE line -- the verification
//! URL and the user code -- to stderr, then polls the token endpoint at the
//! server's interval (slower on slow_down) until the user finishes, the code
//! expires, `timeout_seconds` passes, or `interrupted()` turns true.
DelegatedTokens RunDeviceCodeFlow(const DelegatedFlowConfig &cfg, const std::function<bool()> &interrupted);

//! Authorization code + PKCE (S256) with a localhost redirect, using
//! datazoo-oauth2's loopback server and browser launcher (as gdrive does). The
//! code is redeemed through the raw TLS transport. Fails fast when no browser
//! can be launched.
DelegatedTokens RunAuthorizationCodeFlow(const DelegatedFlowConfig &cfg);

//! The delegated fields of the secret `snapshot` was resolved from, as the
//! secret manager holds them NOW. `present` is false when that secret was
//! dropped, or replaced by one whose configuration differs from the snapshot
//! (any field other than refresh_token / refresh_token_lineage, the provider,
//! or the SCOPE paths).
struct StoredDelegatedState {
	bool present = false;
	string refresh_token;
	string lineage;
};
StoredDelegatedState RefetchDelegatedState(ClientContext &context, const SecretMatch &snapshot);

//! Guarded write-back: store `refresh_token` (and its `lineage`) into the
//! secret `snapshot` was resolved from -- on disk for a PERSISTENT secret --
//! ONLY if that secret still has the snapshot's configuration and still holds
//! `expected_current` as its refresh token. Anything else means it was
//! replaced, dropped or already updated meanwhile, and is left alone (returns
//! false). Only the refresh token is stored: never an access token. The caller
//! serialises write-backs for one secret.
bool WriteBackRefreshToken(ClientContext &context, const SecretMatch &snapshot, const string &expected_current,
                           const string &refresh_token, const string &lineage);
} // namespace sharepoint
} // namespace duckdb
