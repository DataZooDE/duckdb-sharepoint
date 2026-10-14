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

//! Write `refresh_token` into the secret `match` resolved to, keeping its
//! persistence (a PERSISTENT secret is rewritten on disk) and storage. Only
//! the refresh token is stored: an access token is never written to disk.
void PersistRefreshToken(ClientContext &context, const SecretMatch &match, const string &refresh_token);

} // namespace sharepoint
} // namespace duckdb
