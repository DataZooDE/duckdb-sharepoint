#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Delegated (user) auth against the Microsoft identity platform v2 -- the pure
// half. PURE: no DuckDB, no I/O. Request bodies, response parsing, error
// classification and the RFC 8628 polling rules live here so the Catch2 layer
// covers them; sharepoint_delegated_flow.cpp only moves the bytes.
//
// Both delegated providers are PUBLIC clients: no client secret is ever sent.
// The Entra app must have "Allow public client flows" enabled (device code)
// and a "Mobile and desktop applications" redirect of http://localhost
// (authorization code; Entra matches any port on a localhost redirect).
// ---------------------------------------------------------------------------

using FormParams = std::vector<std::pair<std::string, std::string>>;

//! application/x-www-form-urlencoded, in the given order. RFC 3986 unreserved
//! bytes pass through; everything else (including space) is %XX-encoded.
std::string FormUrlEncode(const FormParams &params);

//! The delegated scope for the Graph cloud `graph_endpoint` belongs to:
//! "<graph>/Files.ReadWrite.All <graph>/Sites.ReadWrite.All offline_access".
//! offline_access is what makes Entra issue a refresh token at all.
std::string DelegatedGraphScope(const std::string &graph_endpoint);

//! "<login>/<tenant>/oauth2/v2.0/<leaf>" -- leaf is "token", "devicecode" or
//! "authorize". Trailing slashes on `login_endpoint` are dropped. The caller
//! has already checked the endpoint (IsAllowedLoginEndpoint) and the tenant
//! (IsValidTenantId).
std::string EntraUrl(const std::string &login_endpoint, const std::string &tenant, const std::string &leaf);

//! POST /devicecode: client_id + scope.
FormParams DeviceCodeRequest(const std::string &client_id, const std::string &scope);
//! POST /token while polling a device code (RFC 8628 section 3.4).
FormParams DeviceCodeTokenRequest(const std::string &client_id, const std::string &device_code);
//! POST /token to redeem an authorization code with its PKCE verifier.
FormParams AuthorizationCodeTokenRequest(const std::string &client_id, const std::string &code,
                                         const std::string &redirect_uri, const std::string &code_verifier,
                                         const std::string &scope);
//! POST /token with grant_type=refresh_token. `client_secret` is included only
//! when non-empty: a public client must NOT send one (Entra rejects it).
FormParams RefreshTokenRequest(const std::string &client_id, const std::string &client_secret,
                               const std::string &refresh_token, const std::string &scope);

//! Whether a credential acts as a USER (delegated): the device_code and
//! authorization_code providers, and a config refresh token without a client
//! secret (only a public client issues those). Picks the default scope and the
//! wording of refresh errors -- one rule, so the two cannot drift.
bool IsDelegatedCredential(const std::string &provider, bool has_client_secret);

//! A device-code user_code fit to show a human: 1-16 of [A-Za-z0-9-]. Refuses
//! anything that could break the one-line prompt (newline, ANSI escapes).
bool IsValidUserCode(const std::string &user_code);

//! A verification_uri fit to show a human: https, no userinfo or port, no
//! whitespace / control / non-ASCII bytes, on a Microsoft sign-in domain
//! (microsoft.com, microsoftonline.com/.us/.cn, chinacloudapi.cn or a
//! subdomain). The login host is already allow-listed and TLS-verified; this
//! keeps a broken or hostile reply from steering the user to a phishing page.
bool IsAllowedVerificationUri(const std::string &uri);

//! The /devicecode response (RFC 8628 section 3.2).
struct DeviceCodeGrant {
	std::string device_code;
	std::string user_code;
	std::string verification_uri;
	int64_t expires_in = 0;
	int64_t interval = 0;
};

//! Parse a 200 /devicecode body. False (with a credential-free `error`) when
//! a field is missing, the expiry is not positive, or user_code /
//! verification_uri fail IsValidUserCode / IsAllowedVerificationUri -- both
//! are shown to a human.
bool ParseDeviceCodeResponse(const std::string &body, DeviceCodeGrant &out, std::string &error);

//! What a /token (or /devicecode) reply means for the caller.
enum class TokenReplyKind {
	SUCCESS,
	//! RFC 8628: the user has not finished yet -- keep polling.
	AUTHORIZATION_PENDING,
	//! RFC 8628: poll slower (interval += 5 s) and keep polling.
	SLOW_DOWN,
	//! The device code expired before the user finished.
	EXPIRED_TOKEN,
	//! The user (or policy) declined the sign-in.
	DECLINED,
	//! The app's permissions were not consented (AADSTS65001).
	CONSENT_REQUIRED,
	//! Conditional access / MFA wants an interactive sign-in again.
	INTERACTION_REQUIRED,
	//! Entra wants a client secret: "Allow public client flows" is off
	//! (AADSTS7000218).
	PUBLIC_CLIENT_DISABLED,
	//! The refresh token (or code) is expired, revoked or malformed.
	INVALID_GRANT,
	//! Unknown app, wrong tenant, bad scope, bad redirect URI.
	INVALID_CLIENT,
	//! 429 / 5xx -- try again later.
	TRANSIENT,
	//! Anything else, including an unparseable body.
	OTHER,
};

struct TokenReply {
	TokenReplyKind kind = TokenReplyKind::OTHER;
	std::string access_token;
	std::string refresh_token;
	int64_t expires_in = 0;
	//! Entra's short `error` (e.g. "invalid_grant"); never the description.
	std::string error;
	//! First of `error_codes` (the AADSTS number), 0 if absent.
	int64_t aadsts = 0;
};

//! Classify a token-endpoint reply. A 2xx without an access_token is OTHER.
TokenReply ParseTokenReply(int http_status, const std::string &body);

//! RFC 8628 section 3.2: an absent or non-positive interval means 5 s.
//! Clamped to [1, 60] so a broken server cannot spin us or park us.
int64_t InitialPollIntervalSeconds(int64_t server_interval);
//! The interval after a reply: SLOW_DOWN adds 5 s (RFC 8628 section 3.5),
//! everything else keeps it. Same clamp.
int64_t NextPollIntervalSeconds(int64_t current, TokenReplyKind kind);

//! The one line shown to the user: the verification URL and the user code.
//! Never the device code (that is the credential being polled for).
std::string DeviceCodePrompt(const DeviceCodeGrant &grant);

//! A teaching, credential-free message for a failed delegated sign-in or
//! refresh: "sharepoint secret '<name>' (PROVIDER <provider>): ...".
std::string DelegatedErrorMessage(const TokenReply &reply, const std::string &secret_name,
                                  const std::string &provider);

//! REDIRECT_PORT: empty means 8020 (as gdrive); otherwise 1024..65535.
bool ParseRedirectPort(const std::string &text, int &port);

//! Whether an interactive browser can be launched (same rule as gdrive): on a
//! non-Apple POSIX host, only with an X11 or Wayland display.
bool CanLaunchBrowser(bool is_posix_non_apple, const std::string &display, const std::string &wayland_display);

} // namespace sharepoint
} // namespace duckdb
