// Token acquisition + caching for the `sharepoint` secret.
//
// Proven by the live SQL suite (`make test_live`) against a real tenant.
//
// Every cache here is keyed by a SHA-256 fingerprint of the credential
// material -- never by the secret's name: a temporary secret is per
// connection, so one name can mean different credentials, and a name-keyed
// cache would serve one principal's token to another (the bug the sibling
// gdrive documents and fixed the same way).
#include "sharepoint_auth.hpp"
#include "sharepoint_credential_chain.hpp"
#include "sharepoint_assertion.hpp"
#include "sharepoint_certificate.hpp"
#include "sharepoint_token_policy.hpp"
#include "sharepoint_client.hpp"
#include "sharepoint_counters.hpp"
#include "sharepoint_delegated.hpp"
#include "sharepoint_delegated_flow.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_identity.hpp"
#include "sharepoint_secret.hpp"
#include "sharepoint_weburl.hpp"
#include "sharepoint_uri.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <picojson.h>

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <fstream>
#include <random>
#include <sstream>
#include <cstdint>
#include <functional>
#include "duckdb_compat.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

// Cached bearer token, valid until `expires_at`. Keyed by the credential
// fingerprint (see Fingerprint), so one credential set's token is never
// served to another -- whatever the secrets are named.
struct CachedToken {
	string access_token;
	std::chrono::system_clock::time_point expires_at;
	std::chrono::system_clock::time_point minted_at;
};

mutex g_token_lock;
unordered_map<string, CachedToken> g_tokens;  // credential fingerprint -> token
unordered_map<string, string> g_site_drives; // fingerprint of (credentials, site) -> drive id
//! Browser URLs (#95): a site's document libraries (drive id, webUrl), and
//! what a sharing link points at ("drive id|item id"), per credentials.
unordered_map<string, vector<pair<string, string>>> g_site_libraries;
unordered_map<string, string> g_shared_items;
//! Credential fingerprint -> the newest refresh token Entra ID issued for it.
//! Entra rotates the refresh token on every refresh grant; every later mint
//! for that credential presents the newest one (never a superseded token), and
//! GetAuthContext writes it back into the secret. Holds refresh tokens; never
//! displayed.
unordered_map<string, string> g_latest_refresh;

//! SHA-256 (hex) of length-prefixed credential material. Hashed so no
//! credential sits in a map key, and a cryptographic hash so distinct
//! credentials cannot collide into a shared token.
string Fingerprint(const vector<string> &parts) {
	const string framed = FrameCredentialMaterial(parts);
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	if (EVP_Digest(framed.data(), framed.size(), digest, &digest_len, EVP_sha256(), nullptr) != 1) {
		// Never fail open onto a weaker key -- that is the cross-tenant bug.
		throw IOException("sharepoint: could not fingerprint the credentials (SHA-256 unavailable)");
	}
	static const char *kHex = "0123456789abcdef";
	string hex;
	hex.reserve(digest_len * 2);
	for (unsigned int i = 0; i < digest_len; i++) {
		hex.push_back(kHex[(digest[i] >> 4) & 0xF]);
		hex.push_back(kHex[digest[i] & 0xF]);
	}
	return hex;
}

//! A delegated secret's cache identity: SHA-256 over the refresh token it was
//! FIRST given (REFRESH_TOKEN at creation, or the sign-in's). Stored beside the
//! token as refresh_token_lineage when a token is written back, so the
//! credential fingerprint survives rotation -- keying on the CURRENT token
//! would orphan the token cache, the mint lock and the drive-identity keys on
//! every refresh. Still per user: no two sign-ins share a first token.
string LineageOf(const string &refresh_token) {
	return refresh_token.empty() ? string() : Fingerprint({"refresh-token-lineage", refresh_token});
}

// Read a field from a KeyValueSecret, empty string if absent.
string SecretField(const KeyValueSecret &secret, const char *key) {
	Value v;
	if (secret.TryGetValue(key, v) && !v.IsNull()) {
		return v.ToString();
	}
	return string();
}

// httplib client for a base URL like "https://login.microsoftonline.com".
// Split into host and honour the default 443. cpp-httplib's SSLClient wants
// scheme+host; the path is passed per-request.
duckdb_httplib_openssl::Client MakeClient(const string &base_url) {
	duckdb_httplib_openssl::Client cli(base_url.c_str());
	// The token endpoint never redirects; never re-send client_secret elsewhere.
	cli.set_follow_location(false);
	cli.set_connection_timeout(30);
	cli.set_read_timeout(120);
	return cli;
}

// POST an application/x-www-form-urlencoded body to the Azure AD token
// endpoint and return the parsed JSON, throwing a classified DuckDB exception
// on any non-2xx.
//! Builds the exception text for a non-2xx token reply (status, body).
using TokenErrorFormatter = std::function<string(int, const string &)>;

picojson::object PostToken(const string &login_endpoint, const string &tenant,
                           const duckdb_httplib_openssl::Params &params, const TokenErrorFormatter &on_error) {
	IncrementCounter(Counter::TOKEN_REQUEST);
	auto cli = MakeClient(login_endpoint);
	string path = "/" + tenant + "/oauth2/v2.0/token";
	// Throttling / 5xx / transport failures on the token endpoint are retried
	// like Graph calls: a cold 16-thread scan is exactly when one shows up.
	duckdb_httplib_openssl::Result res;
	for (int attempt = 1;; attempt++) {
		res = cli.Post(path.c_str(), params);
		const bool retryable =
		    !res || res->status == 429 || res->status >= 500;
		if (!retryable || attempt >= kMaxGraphAttempts) {
			break;
		}
		SleepBeforeRetry(attempt, res ? ParseRetryAfterSeconds(res->get_header_value("Retry-After")) : 0);
	}
	if (!res) {
		throw IOException("sharepoint: token request to %s%s failed at the transport layer",
		                  login_endpoint.c_str(), path.c_str());
	}
	if ((res->status < 200 || res->status >= 300) && on_error) {
		throw IOException(on_error(res->status, res->body));
	}
	if (res->status < 200 || res->status >= 300) {
		// Entra ID returns { "error": "invalid_client", "error_codes": [7000215],
		// "error_description": "..." }. Surface the short `error` and the AADSTS
		// code -- enough to look the failure up -- but not the description,
		// which can echo configuration detail.
		string aad_error, aad_code;
		picojson::value ev;
		if (picojson::parse(ev, res->body).empty() && ev.is<picojson::object>()) {
			auto &eo = ev.get<picojson::object>();
			auto e = eo.find("error");
			if (e != eo.end() && e->second.is<string>()) {
				aad_error = e->second.get<string>();
			}
			auto codes = eo.find("error_codes");
			if (codes != eo.end() && codes->second.is<picojson::array>() &&
			    !codes->second.get<picojson::array>().empty() &&
			    codes->second.get<picojson::array>()[0].is<int64_t>()) {
				aad_code = " AADSTS" + std::to_string(codes->second.get<picojson::array>()[0].get<int64_t>());
			}
		}
		throw IOException("sharepoint: token endpoint returned HTTP %d (%s%s). Check TENANT_ID, CLIENT_ID and "
		                  "CLIENT_SECRET, and that the app has admin-consented application permissions.",
		                  res->status, aad_error.empty() ? "error" : aad_error.c_str(), aad_code.c_str());
	}
	picojson::value v;
	string err = picojson::parse(v, res->body);
	if (!err.empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: could not parse the token endpoint response as JSON");
	}
	return v.get<picojson::object>();
}

//! What it takes to mint a token for one credential set, and to cache it.
struct MintRequest {
	string credential_fp;
	string secret_name;
	string login_endpoint;
	string tenant;
	string provider;
	string client_id;
	string client_secret;
	//! PROVIDER client_certificate: the PEM (certificate + key) and the key's
	//! password; a fresh signed assertion goes with every token request.
	string certificate_pem;
	string certificate_password;
	string refresh_token;
	string scope;
	//! Test hook (sharepoint_debug_token_max_age_seconds): treat tokens as
	//! expiring after at most this many seconds, so expiry can be exercised
	//! without waiting an hour. 0 = Entra ID's own lifetime.
	int64_t max_age_seconds = 0;
};

//! A signed client assertion for a certificate credential (#71): good for ten
//! minutes, for this tenant's token endpoint only, with a fresh jti.
string ClientAssertion(const MintRequest &req) {
	string x5t, signature, error;
	if (!CertificateThumbprint(req.certificate_pem, x5t, error)) {
		throw InvalidInputException("sharepoint: secret '%s': %s", req.secret_name, error);
	}
	std::random_device rd;
	std::ostringstream jti;
	jti << std::hex << rd() << rd() << rd() << rd();
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
	                     std::chrono::system_clock::now().time_since_epoch())
	                     .count();
	const auto input = ClientAssertionSigningInput(
	    req.client_id, req.login_endpoint + "/" + req.tenant + "/oauth2/v2.0/token", x5t, jti.str(), now);
	if (!SignRs256(req.certificate_pem, req.certificate_password, input, signature, error)) {
		throw InvalidInputException("sharepoint: secret '%s': %s", req.secret_name, error);
	}
	return input + "." + Base64Url(signature);
}

//! Mint a token and store it in the shared cache. Throttling and 5xx on the
//! token endpoint are retried with the same backoff as Graph calls.
string MintToken(const MintRequest &req) {
	duckdb_httplib_openssl::Params params;
	TokenErrorFormatter on_error;
	string refresh_token = req.refresh_token;
	if (!refresh_token.empty()) {
		// Present the newest token Entra issued for this credential, not the
		// one this source was built with (it may have been superseded).
		{
			lock_guard<mutex> guard(g_token_lock);
			auto latest = g_latest_refresh.find(req.credential_fp);
			if (latest != g_latest_refresh.end()) {
				refresh_token = latest->second;
			}
		}
		// PROVIDER config, device_code and authorization_code: the refresh
		// grant. A public client (no CLIENT_SECRET) sends none.
		for (auto &kv : RefreshTokenRequest(req.client_id, req.client_secret, refresh_token, req.scope)) {
			params.emplace(kv.first, kv.second);
		}
		if (IsDelegatedCredential(req.provider, !req.client_secret.empty())) {
			// A delegated token: say what fixes a delegated failure (consent,
			// public client flows, sign in again), not the app-only checklist.
			const string name = req.secret_name, provider = req.provider;
			on_error = [name, provider](int status, const string &body) {
				return DelegatedErrorMessage(ParseTokenReply(status, body), name, provider);
			};
		}
	} else if (!req.certificate_pem.empty()) {
		params.emplace("client_id", req.client_id);
		params.emplace("client_assertion_type", "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
		params.emplace("client_assertion", ClientAssertion(req));
		params.emplace("scope", req.scope);
		params.emplace("grant_type", "client_credentials");
	} else {
		params.emplace("client_id", req.client_id);
		params.emplace("client_secret", req.client_secret);
		params.emplace("scope", req.scope);
		params.emplace("grant_type", "client_credentials");
	}
	auto obj = PostToken(req.login_endpoint, req.tenant, params, on_error);
	auto at = obj.find("access_token");
	if (at == obj.end() || !at->second.is<string>()) {
		throw IOException("sharepoint: token endpoint returned no access_token");
	}
	const string token = at->second.get<string>();
	int64_t expires_in = 3600;
	auto ei = obj.find("expires_in");
	if (ei != obj.end() && ei->second.is<int64_t>()) {
		expires_in = ei->second.get<int64_t>();
	} else if (ei != obj.end() && ei->second.is<string>()) {
		// Some Entra ID endpoints send it as a string.
		char *end = nullptr;
		const auto parsed = std::strtoll(ei->second.get<string>().c_str(), &end, 10);
		if (end && *end == '\0' && parsed > 0) {
			expires_in = parsed;
		}
	}
	if (req.max_age_seconds > 0) {
		expires_in = std::min<int64_t>(expires_in, req.max_age_seconds);
	}
	lock_guard<mutex> guard(g_token_lock);
	const auto now = std::chrono::system_clock::now();
	g_tokens[req.credential_fp] = CachedToken {token, now + std::chrono::seconds(expires_in), now};
	auto rotated = obj.find("refresh_token");
	if (!refresh_token.empty() && rotated != obj.end() && rotated->second.is<string>() &&
	    !rotated->second.get<string>().empty() && rotated->second.get<string>() != refresh_token) {
		g_latest_refresh[req.credential_fp] = rotated->second.get<string>();
	}
	return token;
}

//! One mint lock per credential fingerprint, shared by every source for it:
//! N open handles on one credential must mint ONE token on expiry, not N.
shared_ptr<mutex> MintLockFor(const string &credential_fp) {
	static mutex locks_guard;
	static unordered_map<string, shared_ptr<mutex>> locks;
	lock_guard<mutex> guard(locks_guard);
	auto &lock = locks[credential_fp];
	if (!lock) {
		lock = make_shared_ptr<mutex>();
	}
	return lock;
}

//! Client-credentials / refresh-token secrets: cached per credential
//! fingerprint, re-minted a minute before expiry or on demand after a 401.
class MintingTokenSource : public TokenSource {
public:
	explicit MintingTokenSource(MintRequest req_p)
	    : req(std::move(req_p)), mint_lock(MintLockFor(req.credential_fp)) {
	}
	string Token() override {
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(req.credential_fp);
			if (it != g_tokens.end() && Fresh(it->second)) {
				return it->second.access_token;
			}
		}
		// One mint at a time per credential: concurrent handles and block
		// fetches share it instead of each POSTing to the token endpoint.
		lock_guard<mutex> mint_guard(*mint_lock);
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(req.credential_fp);
			if (it != g_tokens.end() && Fresh(it->second)) {
				return it->second.access_token;
			}
		}
		return MintToken(req);
	}
	//! `rejected` is the token the server just refused. If another thread has
	//! already replaced it, that newer token is used instead of minting again.
	string Refresh(const string &rejected) override {
		lock_guard<mutex> mint_guard(*mint_lock);
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(req.credential_fp);
			if (it != g_tokens.end() && it->second.access_token != rejected && Fresh(it->second)) {
				return it->second.access_token;
			}
			g_tokens.erase(req.credential_fp);
		}
		return MintToken(req);
	}

private:
	//! Valid for at least another minute -- by Entra ID's expiry and, under
	//! the test cap, by age (a token cached before the cap was set counts too).
	bool Fresh(const CachedToken &t) const {
		using std::chrono::system_clock;
		return TokenIsFresh(system_clock::to_time_t(t.minted_at), system_clock::to_time_t(t.expires_at),
		                    system_clock::to_time_t(system_clock::now()), req.max_age_seconds);
	}

	MintRequest req;
	shared_ptr<mutex> mint_lock;
};

//! PROVIDER config with a bare ACCESS_TOKEN: the caller owns its lifetime.
class StaticTokenSource : public TokenSource {
public:
	explicit StaticTokenSource(string token_p) : token(std::move(token_p)) {
	}
	string Token() override {
		return token;
	}
	string Refresh(const string &) override {
		return token; // nothing to refresh with; the 401 will surface
	}

private:
	string token;
};

// The `sharepoint` secret in scope for `path`, or throw a teaching error.
//
// Returns the SecretMatch BY VALUE and the caller must keep it alive while it
// reads the secret: the match owns a deep clone of the SecretEntry (a
// unique_ptr), so a reference into it dangles the moment the match is
// destroyed. Returning `const KeyValueSecret &` here was a use-after-free that
// surfaced on the first live run as "the secret gives neither DRIVE_ID nor
// SITE_ID" for a secret that plainly had one.
SecretMatch LookupSecret(optional_ptr<FileOpener> opener, const string &path) {
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!secret_manager || !transaction) {
		throw InvalidInputException("sharepoint: '%s' was opened without a database context, so no secret can be "
		                            "looked up",
		                            path);
	}
	auto match = secret_manager->LookupSecret(*transaction, path, kSecretTypeName);
	if (!match.HasMatch()) {
		// A secret may EXIST and still not apply: DuckDB's SCOPE clause limits
		// which paths a secret covers, and SCOPE is easy to reach for when the
		// OAuth scope (GRAPH_SCOPE) was meant. "No secret configured" would send
		// that user off to create a second one that does not match either.
		if (HasAnySharepointSecret(opener)) {
			throw InvalidInputException(
			    "sharepoint: a `sharepoint` secret exists, but none applies to '%s'. A secret's SCOPE clause "
			    "limits which paths it covers (the OAuth scope option is GRAPH_SCOPE, not SCOPE). Check: "
			    "SELECT name, scope FROM duckdb_secrets();",
			    path);
		}
		throw InvalidInputException(
		    "sharepoint: no `sharepoint` secret configured for '%s'. Create one, e.g.\n"
		    "  CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,\n"
		    "                    TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...', SITE_URL 'https://contoso.sharepoint.com/sites/Sales');",
		    path);
	}
	return match;
}

// GET `path` on Graph with the usual retries; the reply's JSON object, or
// throw naming `what`. A 404 says `not_found` instead.
picojson::object GraphGetObject(const string &graph_endpoint, const string &token, const string &path,
                                const string &what, const string &not_found) {
	auto cli = MakeClient(graph_endpoint);
	duckdb_httplib_openssl::Headers headers = {{"Authorization", "Bearer " + token}};
	duckdb_httplib_openssl::Result res;
	for (int attempt = 1;; attempt++) {
		res = cli.Get(path.c_str(), headers);
		const bool retryable = !res || res->status == 429 || res->status >= 500;
		if (!retryable || attempt >= kMaxGraphAttempts) {
			break;
		}
		SleepBeforeRetry(attempt, res ? ParseRetryAfterSeconds(res->get_header_value("Retry-After")) : 0);
	}
	if (res && res->status == 404 && !not_found.empty()) {
		throw InvalidInputException("sharepoint: %s", not_found);
	}
	if (!res || res->status < 200 || res->status >= 300) {
		throw IOException("sharepoint: could not resolve %s (HTTP %d)", what, res ? res->status : 0);
	}
	picojson::value v;
	if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected response resolving %s", what);
	}
	return v.get<picojson::object>();
}

string JsonString(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<string>() ? it->second.get<string>() : string();
}

// The Graph site id of a SITE_URL ("https://contoso.sharepoint.com/sites/Sales").
string SiteIdOfUrl(const SharepointWebUrl &url, const string &site_url, const string &graph_endpoint,
                   const string &token) {
	string address = url.host;
	if (!url.site_path.empty()) {
		address += ":";
		for (auto &segment : StringUtil::Split(url.site_path, '/')) {
			address += "/" + PercentEncodeSegment(segment);
		}
	}
	const string what = "site '" + site_url + "'";
	auto site = GraphGetObject(graph_endpoint, token, "/v1.0/sites/" + address + "?$select=id", what,
	                           what + " not found -- check the address, or, for an app with Sites.Selected, grant "
	                                  "it this site (README: Least privilege)");
	auto id = JsonString(site, "id");
	if (id.empty()) {
		throw IOException("sharepoint: %s has no id", what);
	}
	return id;
}

// Resolve the drive the paths hang off. Prefer an explicit DRIVE_ID; else the
// site's (SITE_ID, or SITE_URL from the browser) default document library, or
// the one named DRIVE, or -- for a SITE_URL that goes on into a library --
// that library. Cached per credentials + site + library. Without any of them,
// "" when `require_drive` is false (sharepoint_sites() needs no drive).
string ResolveDriveId(const KeyValueSecret &secret, const string &graph_endpoint, const string &token,
                      const string &credential_fp, bool require_drive) {
	string drive_id = SecretField(secret, "drive_id");
	if (!drive_id.empty()) {
		return drive_id;
	}
	string site_id = SecretField(secret, "site_id");
	const string site_url = SecretField(secret, "site_url");
	const string drive_name = SecretField(secret, "drive");
	if (site_id.empty() && site_url.empty()) {
		if (!require_drive) {
			return string();
		}
		throw InvalidInputException(
		    "sharepoint: the secret gives no SITE_URL, DRIVE_ID or SITE_ID, so there is no drive to address. "
		    "Add the site's address from the browser, e.g. SITE_URL 'https://contoso.sharepoint.com/sites/Sales' "
		    "(SELECT * FROM sharepoint_sites() lists the sites you can reach).");
	}
	// A site's libraries do not move; resolve once per (credentials, cloud,
	// site, library) instead of extra Graph calls per file operation.
	const string cache_key = Fingerprint({credential_fp, graph_endpoint, site_id, site_url, drive_name});
	{
		lock_guard<mutex> guard(g_token_lock);
		auto it = g_site_drives.find(cache_key);
		if (it != g_site_drives.end()) {
			return it->second;
		}
	}
	SharepointWebUrl url;
	if (site_id.empty()) {
		url = ParseSharepointWebUrl(site_url).url; // validated by CREATE SECRET
		site_id = SiteIdOfUrl(url, site_url, graph_endpoint, token);
	}
	const string site_what = site_url.empty() ? "SITE_ID '" + site_id + "'" : "SITE_URL '" + site_url + "'";
	if (!drive_name.empty() || !url.rest.empty()) {
		auto drives = GraphGetObject(graph_endpoint, token, "/v1.0/sites/" + site_id + "/drives?$select=id,name,webUrl",
		                             "the document libraries of " + site_what, string());
		string names;
		int best = -1;
		auto values = drives.find("value");
		if (values != drives.end() && values->second.is<picojson::array>()) {
			for (auto &d : values->second.get<picojson::array>()) {
				if (!d.is<picojson::object>()) {
					continue;
				}
				const auto &o = d.get<picojson::object>();
				const auto name = JsonString(o, "name");
				names += (names.empty() ? "" : ", ") + string("'") + name + "'";
				if (!drive_name.empty()) {
					if (StringUtil::CIEquals(name, drive_name)) {
						drive_id = JsonString(o, "id");
					}
				} else {
					const int covered = MatchDriveWebUrl(url, JsonString(o, "webUrl"));
					if (covered > best) {
						best = covered;
						drive_id = JsonString(o, "id");
					}
				}
			}
		}
		if (drive_id.empty()) {
			throw InvalidInputException("sharepoint: %s has no document library %s; it has %s", site_what,
			                            drive_name.empty() ? string("at that address") : "named '" + drive_name + "'",
			                            names.empty() ? string("none") : names);
		}
	} else {
		auto drive = GraphGetObject(graph_endpoint, token, "/v1.0/sites/" + site_id + "/drive?$select=id",
		                            "the default document library of " + site_what, string());
		drive_id = JsonString(drive, "id");
		if (drive_id.empty()) {
			throw IOException("sharepoint: the default document library of %s has no id", site_what);
		}
	}
	lock_guard<mutex> guard(g_token_lock);
	g_site_drives[cache_key] = drive_id;
	return drive_id;
}

struct SignInResult {
	string refresh_token;
	string lineage;
};

//! Run the delegated sign-in for a secret that has no refresh token yet, store
//! the refresh token back into the secret (guarded: see WriteBackRefreshToken),
//! and seed the token cache with the access token it came with.
//!
//! Serialised per secret: a multi-file scan opens files on several threads at
//! once, and each must not print its own device code. Under the lock the
//! secret is RE-READ from the secret manager -- no side cache -- so the threads
//! that waited find the refresh token the first one stored, while a secret
//! recreated without REFRESH_TOKEN (the documented recovery) or for another
//! user really does sign in again.
SignInResult SignIn(optional_ptr<FileOpener> opener, const SecretMatch &match, const string &path,
                    const string &provider, const string &login_endpoint, const string &tenant,
                    const string &client_id, const string &scope,
                    const std::function<string(const string &)> &fingerprint_for) {
	const string secret_name = duckdb_compat::Str(match.GetSecret().GetName());
	auto client_context = FileOpener::TryGetClientContext(opener);
	if (!client_context) {
		throw IOException("sharepoint secret '%s' (PROVIDER %s) has no refresh token yet, and '%s' was opened "
		                  "without a client connection (e.g. by ATTACH), so it cannot sign in or store the result. "
		                  "Sign in first with any query on a sharepoint:// path from a regular connection, or pass "
		                  "REFRESH_TOKEN.",
		                  secret_name, provider, path);
	}
	// One sign-in at a time per secret (it can wait minutes for a human);
	// other secrets are not held up.
	auto signin_lock = MintLockFor(Fingerprint({"signin", secret_name, match.secret_entry->storage_mode, provider,
	                                            tenant, client_id, scope, login_endpoint}));
	lock_guard<mutex> signin_guard(*signin_lock);

	const auto stored = RefetchDelegatedState(*client_context, match);
	if (!stored.present) {
		throw IOException("sharepoint secret '%s' was replaced or dropped while '%s' was being opened; run the "
		                  "statement again.",
		                  secret_name, path);
	}
	if (!stored.refresh_token.empty()) {
		// Another thread signed in while this one waited for the lock.
		return SignInResult {stored.refresh_token,
		                     stored.lineage.empty() ? LineageOf(stored.refresh_token) : stored.lineage};
	}

	DelegatedFlowConfig cfg;
	cfg.secret_name = secret_name;
	cfg.provider = provider;
	cfg.login_endpoint = login_endpoint;
	cfg.tenant = tenant;
	cfg.client_id = client_id;
	cfg.scope = scope;
	DelegatedTokens tokens;
	if (provider == "device_code") {
		Value timeout;
		if (FileOpener::TryGetCurrentSetting(opener, "sharepoint_device_code_timeout_seconds", timeout) &&
		    !timeout.IsNull()) {
			cfg.timeout_seconds = timeout.GetValue<int64_t>();
		}
		ClientContext &ctx = *client_context;
		tokens = RunDeviceCodeFlow(cfg, [&ctx]() { return duckdb_compat::IsInterrupted(ctx); });
	} else {
		const auto &kv = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
		int port = 8020;
		ParseRedirectPort(SecretField(kv, "redirect_port"), port); // validated at CREATE SECRET
		cfg.redirect_port = port;
		tokens = RunAuthorizationCodeFlow(cfg);
	}
	if (tokens.refresh_token.empty()) {
		throw IOException("sharepoint secret '%s' (PROVIDER %s): the sign-in succeeded but Entra ID issued no "
		                  "refresh token. GRAPH_SCOPE must include offline_access, and the app must have it "
		                  "consented.",
		                  secret_name, provider);
	}

	// Store the refresh token in the secret -- on disk for a PERSISTENT one --
	// so this is the last prompt. Skipped if the secret was replaced or dropped
	// during the sign-in (this statement still uses the tokens). Then cache the
	// access token it came with, so this statement does not mint a second one.
	const string lineage = LineageOf(tokens.refresh_token);
	WriteBackRefreshToken(*client_context, match, string(), tokens.refresh_token, lineage);
	{
		lock_guard<mutex> token_guard(g_token_lock);
		const auto now = std::chrono::system_clock::now();
		g_tokens[fingerprint_for(lineage)] =
		    CachedToken {tokens.access_token, now + std::chrono::seconds(tokens.expires_in), now};
	}
	return SignInResult {tokens.refresh_token, lineage};
}

//! After a mint: if Entra rotated this credential's refresh token, write the
//! newest one back into the secret (guarded), so a restart -- or the stored
//! token's own expiry -- never falls back to a superseded token. Best effort:
//! a statement never fails because the write-back could not happen.
void PersistRotatedRefreshToken(optional_ptr<FileOpener> opener, const SecretMatch &match,
                                const string &credential_fp, const string &stored_refresh_token,
                                const string &lineage) {
	string latest;
	{
		lock_guard<mutex> guard(g_token_lock);
		auto it = g_latest_refresh.find(credential_fp);
		if (it == g_latest_refresh.end() || it->second == stored_refresh_token) {
			return;
		}
		latest = it->second;
	}
	auto client_context = FileOpener::TryGetClientContext(opener);
	if (!client_context) {
		return; // e.g. an ATTACH opener; the next regular statement writes it
	}
	auto lock = MintLockFor(Fingerprint({"writeback", credential_fp}));
	lock_guard<mutex> guard(*lock);
	try {
		WriteBackRefreshToken(*client_context, match, stored_refresh_token, latest, lineage);
	} catch (std::exception &) {
		// The rotated token stays in use in-process; a later statement retries.
	}
}

} // namespace

// ---------------------------------------------------------------------------
// PROVIDER credential_chain (#72): the workload's own Azure identity. The
// order and parsing are sharepoint_credential_chain.*; the requests are here.
// ---------------------------------------------------------------------------

string EnvOr(const char *name) {
	const char *v = std::getenv(name);
	return v ? string(v) : string();
}

int64_t NowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

//! The managed identity endpoints are local, plain http and quick or absent:
//! a short connect timeout keeps the chain moving where there is none.
ChainToken ManagedIdentityToken(ChainSource source, const ChainEnv &env, const string &resource,
                                const string &client_id) {
	string base, path;
	duckdb_httplib_openssl::Headers headers;
	if (source == ChainSource::APP_SERVICE) {
		const auto scheme_end = env.identity_endpoint.find("://");
		const auto path_at =
		    env.identity_endpoint.find('/', scheme_end == string::npos ? 0 : scheme_end + 3);
		base = env.identity_endpoint.substr(0, path_at);
		path = path_at == string::npos ? "/" : env.identity_endpoint.substr(path_at);
		headers.emplace("X-IDENTITY-HEADER", env.identity_header);
	} else {
		base = "http://169.254.169.254";
		path = "/metadata/identity/oauth2/token";
		headers.emplace("Metadata", "true");
	}
	duckdb_httplib_openssl::Client cli(base.c_str());
	cli.set_connection_timeout(1);
	cli.set_read_timeout(10);
	cli.set_follow_location(false);
	auto res = cli.Get((path + "?" + ManagedIdentityQuery(source, resource, client_id)).c_str(), headers);
	ChainToken t;
	if (!res) {
		t.error = "no endpoint answered";
		return t;
	}
	return ParseManagedIdentityReply(res->status, res->body, NowSeconds());
}

ChainToken AzureCliToken(const string &resource) {
	ChainToken t;
#if defined(__EMSCRIPTEN__)
	t.error = "not available in WebAssembly";
	return t;
#else
	// `resource` is a checked Microsoft Graph host (IsAllowedGraphEndpoint):
	// no shell-active characters.
	const string command = "az account get-access-token --resource " + resource + " --output json 2>&1";
#if defined(_WIN32)
	FILE *pipe = _popen(command.c_str(), "r");
#else
	FILE *pipe = popen(command.c_str(), "r");
#endif
	if (!pipe) {
		t.error = "could not run az";
		return t;
	}
	string output;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
		output.append(buf, n);
	}
#if defined(_WIN32)
	const int rc = _pclose(pipe);
#else
	const int rc = pclose(pipe);
#endif
	t = ParseAzureCliReply(output, NowSeconds());
	if (!t.ok && rc != 0 && output.empty()) {
		t.error = "az is not installed or failed";
	}
	return t;
#endif
}

ChainToken WorkloadIdentityToken(const ChainEnv &env, const string &login_endpoint, const string &resource) {
	ChainToken t;
	std::ifstream in(env.federated_token_file, std::ios::binary);
	std::stringstream buf;
	buf << in.rdbuf();
	string assertion = buf.str();
	while (!assertion.empty() && (assertion.back() == '\n' || assertion.back() == '\r')) {
		assertion.pop_back();
	}
	if (!in || assertion.empty()) {
		t.error = "cannot read AZURE_FEDERATED_TOKEN_FILE";
		return t;
	}
	if (!IsValidTenantId(env.tenant_id)) {
		t.error = "AZURE_TENANT_ID is not a tenant GUID or domain";
		return t;
	}
	duckdb_httplib_openssl::Params params;
	params.emplace("client_id", env.client_id);
	params.emplace("client_assertion_type", "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
	params.emplace("client_assertion", assertion);
	params.emplace("scope", resource + "/.default");
	params.emplace("grant_type", "client_credentials");
	try {
		auto obj = PostToken(login_endpoint, env.tenant_id, params, [](int status, const string &body) {
			return "sharepoint: workload identity token request failed: HTTP " + std::to_string(status) + " " +
			       body.substr(0, 300);
		});
		return ParseManagedIdentityReply(200, picojson::value(obj).serialize(), NowSeconds());
	} catch (std::exception &e) {
		t.error = e.what();
		return t;
	}
}

class ChainTokenSource : public TokenSource {
public:
	ChainTokenSource(string fp_p, string login_endpoint_p, string resource_p, string client_id_p)
	    : fp(std::move(fp_p)), login_endpoint(std::move(login_endpoint_p)), resource(std::move(resource_p)),
	      client_id(std::move(client_id_p)), mint_lock(MintLockFor(fp)) {
	}
	string Token() override {
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(fp);
			if (it != g_tokens.end() && Fresh(it->second)) {
				return it->second.access_token;
			}
		}
		lock_guard<mutex> mint_guard(*mint_lock);
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(fp);
			if (it != g_tokens.end() && Fresh(it->second)) {
				return it->second.access_token;
			}
		}
		return Mint();
	}
	string Refresh(const string &rejected) override {
		lock_guard<mutex> mint_guard(*mint_lock);
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(fp);
			if (it != g_tokens.end() && it->second.access_token != rejected && Fresh(it->second)) {
				return it->second.access_token;
			}
			g_tokens.erase(fp);
		}
		return Mint();
	}

private:
	static bool Fresh(const CachedToken &t) {
		using std::chrono::system_clock;
		return TokenIsFresh(system_clock::to_time_t(t.minted_at), system_clock::to_time_t(t.expires_at),
		                    system_clock::to_time_t(system_clock::now()), 0);
	}

	//! Each source in turn; the first token wins. Every failure is named in
	//! the error when none does.
	string Mint() {
		ChainEnv env;
		env.federated_token_file = EnvOr("AZURE_FEDERATED_TOKEN_FILE");
		env.client_id = EnvOr("AZURE_CLIENT_ID");
		env.tenant_id = EnvOr("AZURE_TENANT_ID");
		env.identity_endpoint = EnvOr("IDENTITY_ENDPOINT");
		env.identity_header = EnvOr("IDENTITY_HEADER");
		string failures;
		for (auto source : ChainOrder(env)) {
			IncrementCounter(Counter::TOKEN_REQUEST);
			ChainToken t;
			switch (source) {
			case ChainSource::WORKLOAD_IDENTITY:
				t = WorkloadIdentityToken(env, login_endpoint, resource);
				break;
			case ChainSource::APP_SERVICE:
			case ChainSource::IMDS:
				t = ManagedIdentityToken(source, env, resource, client_id);
				break;
			default:
				t = AzureCliToken(resource);
				break;
			}
			if (t.ok) {
				lock_guard<mutex> guard(g_token_lock);
				const auto now = std::chrono::system_clock::now();
				g_tokens[fp] = CachedToken {t.token, std::chrono::system_clock::from_time_t(t.expires_on), now};
				return t.token;
			}
			failures += string(failures.empty() ? "" : "; ") + ChainSourceName(source) + ": " + t.error;
		}
		throw IOException("sharepoint: PROVIDER credential_chain found no Azure identity (%s). Run in Azure with a "
		                  "managed identity, set the workload identity variables, or sign in with `az login`.",
		                  failures);
	}

	string fp;
	string login_endpoint;
	string resource;
	string client_id;
	shared_ptr<mutex> mint_lock;
};

namespace {

//! GetAuthContext, or with `rest_host` set (#97) the same credentials for
//! SharePoint's own REST API on that host: only `tokens` and `credential_fp`
//! are filled, no drive is resolved.
AuthContext BuildAuthContext(optional_ptr<FileOpener> opener, const string &path, bool require_drive,
                             const string &rest_host, bool *client_secret_app) {
	const bool rest = !rest_host.empty();
	const auto match = LookupSecret(opener, path);
	const auto &secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());

	string login_endpoint = "https://login.microsoftonline.com";
	string graph_endpoint = "https://graph.microsoft.com";
	{
		Value v;
		if (FileOpener::TryGetCurrentSetting(opener, "sharepoint_login_endpoint", v) && !v.IsNull()) {
			login_endpoint = v.ToString();
		}
		if (FileOpener::TryGetCurrentSetting(opener, "sharepoint_graph_endpoint", v) && !v.IsNull()) {
			graph_endpoint = v.ToString();
		}
	}
	// The endpoints are SESSION settings, so they are checked here, at the point
	// credentials would be sent: only Microsoft cloud hosts over https may
	// receive the client secret or a bearer token.
	if (!IsAllowedLoginEndpoint(login_endpoint)) {
		throw InvalidInputException("sharepoint: sharepoint_login_endpoint '%s' is not a Microsoft Entra ID login "
		                            "host (https://login.microsoftonline.com, .us, or the China cloud)",
		                            login_endpoint);
	}
	if (!IsAllowedGraphEndpoint(graph_endpoint)) {
		throw InvalidInputException("sharepoint: sharepoint_graph_endpoint '%s' is not a Microsoft Graph host "
		                            "(https://graph.microsoft.com, the US Gov / DoD hosts, or the China cloud)",
		                            graph_endpoint);
	}
	while (!graph_endpoint.empty() && graph_endpoint.back() == '/') {
		graph_endpoint.pop_back();
	}
	while (!login_endpoint.empty() && login_endpoint.back() == '/') {
		login_endpoint.pop_back();
	}

	const string provider = duckdb_compat::Str(secret.GetProvider());
	if (provider == "credential_chain") {
		// No credential in the secret: the identity is the workload's. The
		// cache key covers what picks the identity (env and user-assigned id).
		const string mi_client_id = SecretField(secret, "client_id");
		const string resource = rest ? "https://" + rest_host : GraphResource(graph_endpoint);
		const string credential_fp =
		    Fingerprint({provider, mi_client_id, resource, login_endpoint, EnvOr("AZURE_CLIENT_ID"),
		                 EnvOr("AZURE_TENANT_ID"), EnvOr("AZURE_FEDERATED_TOKEN_FILE"), EnvOr("IDENTITY_ENDPOINT"),
		                 EnvOr("AZURE_CONFIG_DIR")});
		AuthContext auth;
		auth.tokens = make_shared_ptr<ChainTokenSource>(credential_fp, login_endpoint, resource, mi_client_id);
		auth.graph_endpoint = graph_endpoint;
		auth.credential_fp = credential_fp;
		if (rest) {
			return auth;
		}
		auth.drive_id = ResolveDriveId(secret, graph_endpoint, auth.tokens->Token(), credential_fp, require_drive);
		auth.identity = Fingerprint({credential_fp, graph_endpoint, auth.drive_id});
		return auth;
	}
	// The providers that sign a user in interactively (and own their secret's
	// refresh token, writing it back).
	const bool interactive = provider == "device_code" || provider == "authorization_code";
	const string tenant = SecretField(secret, "tenant_id");
	const string client_id = SecretField(secret, "client_id");
	// Never read for an interactive provider: a public client has no secret.
	const string client_secret = interactive ? string() : SecretField(secret, "client_secret");
	// PROVIDER client_certificate: the PEM file, read on every use, so a
	// renewed certificate at the same path is picked up (its content is part
	// of the fingerprint below, so it also gets a token of its own).
	string certificate_pem;
	const string certificate_password =
	    provider == "client_certificate" ? SecretField(secret, "certificate_password") : string();
	if (provider == "client_certificate") {
		const string cert_path = SecretField(secret, "certificate_path");
		std::ifstream in(cert_path, std::ios::binary);
		std::stringstream buf;
		buf << in.rdbuf();
		certificate_pem = buf.str();
		string x5t, error;
		if (!in || certificate_pem.empty()) {
			throw InvalidInputException("sharepoint: secret '%s': cannot read CERTIFICATE_PATH '%s'",
			                            duckdb_compat::Str(secret.GetName()), cert_path);
		}
		if (!CertificateThumbprint(certificate_pem, x5t, error)) {
			throw InvalidInputException("sharepoint: secret '%s': CERTIFICATE_PATH '%s': %s",
			                            duckdb_compat::Str(secret.GetName()), cert_path, error);
		}
	}
	string refresh_token = SecretField(secret, "refresh_token");
	string lineage = SecretField(secret, "refresh_token_lineage");
	if (lineage.empty()) {
		lineage = LineageOf(refresh_token);
	}
	const string configured_scope = SecretField(secret, "graph_scope");
	// App-only tokens use "<graph>/.default"; a delegated token names its
	// permissions plus offline_access (or Entra issues no refresh token). A
	// config refresh token without CLIENT_SECRET is a public-client (delegated)
	// one too.
	// SharePoint's REST API: whatever the app is consented for there, app-only
	// or delegated alike (a refresh token redeems for another resource).
	const string scope = rest                                                      ? "https://" + rest_host + "/.default"
	                     : !configured_scope.empty()                                ? configured_scope
	                     : IsDelegatedCredential(provider, !client_secret.empty()) ? DelegatedGraphScope(graph_endpoint)
	                                                                               : DefaultGraphScope(graph_endpoint);
	if (client_secret_app) {
		*client_secret_app = provider == "client_credentials";
	}

	// A config secret with ONLY an ACCESS_TOKEN is used as-is -- the caller owns
	// its lifetime. With a REFRESH_TOKEN as well, the refresh grant is used, so
	// the secret keeps working past the pasted token's expiry.
	const bool can_refresh = !refresh_token.empty() && !tenant.empty() && !client_id.empty();
	const string static_token = (provider == "config" && !can_refresh) ? SecretField(secret, "access_token") : string();

	// Everything that changes which token comes back -- and where it may be
	// sent. Two secrets (or one name recreated) share a token only if all of
	// this is identical. For a refresh-token secret the token's LINEAGE (see
	// LineageOf) is the user's identity: two users' secrets of the same name
	// never share a token, and rotation does not change the key.
	auto fingerprint_for = [&](const string &lin) {
		return Fingerprint({provider, tenant, client_id, client_secret, lin, scope, login_endpoint, static_token,
		                    certificate_pem, certificate_password});
	};
	string credential_fp = fingerprint_for(lineage);

	shared_ptr<TokenSource> tokens;
	if (rest && !static_token.empty()) {
		throw InvalidInputException("sharepoint: secret '%s' holds only an ACCESS_TOKEN, which is for Microsoft Graph; "
		                            "SharePoint's own API (list attachments) needs a token of its own -- give the "
		                            "secret a REFRESH_TOKEN, or use another provider",
		                            duckdb_compat::Str(secret.GetName()));
	}
	if (!static_token.empty()) {
		tokens = make_shared_ptr<StaticTokenSource>(static_token);
	} else {
		if (!IsValidTenantId(tenant)) {
			throw InvalidInputException("sharepoint: TENANT_ID must be a tenant GUID or domain (letters, digits, "
			                            "'-', '.')");
		}
		if (rest && interactive && refresh_token.empty()) {
			// GetAuthContext signs in first (the caller resolves the list through Graph).
			throw IOException("sharepoint secret '%s' has no refresh token yet: run any query on a sharepoint:// path "
			                  "first, to sign in",
			                  duckdb_compat::Str(secret.GetName()));
		}
		if (interactive && refresh_token.empty()) {
			// No refresh token yet: the interactive sign-in, once. Afterwards the
			// refresh token lives in the secret and this branch is not taken.
			const auto signed_in = SignIn(opener, match, path, provider, login_endpoint, tenant, client_id, scope,
			                              fingerprint_for);
			refresh_token = signed_in.refresh_token;
			lineage = signed_in.lineage;
			credential_fp = fingerprint_for(lineage);
		}
		MintRequest req;
		req.credential_fp = credential_fp;
		req.secret_name = duckdb_compat::Str(secret.GetName());
		req.login_endpoint = login_endpoint;
		req.tenant = tenant;
		req.provider = provider;
		req.client_id = client_id;
		req.client_secret = client_secret;
		req.certificate_pem = certificate_pem;
		req.certificate_password = certificate_password;
		req.refresh_token = refresh_token;
		req.scope = scope;
		Value max_age;
		if (FileOpener::TryGetCurrentSetting(opener, "sharepoint_debug_token_max_age_seconds", max_age) &&
		    !max_age.IsNull()) {
			if (!NormalizeMaxAgeSeconds(max_age.GetValue<int64_t>(), req.max_age_seconds)) {
				throw InvalidInputException("sharepoint_debug_token_max_age_seconds must be 0 (no cap) or positive");
			}
		}
		tokens = make_shared_ptr<MintingTokenSource>(std::move(req));
	}
	if (rest) {
		AuthContext auth;
		auth.tokens = tokens;
		auth.graph_endpoint = graph_endpoint;
		auth.credential_fp = credential_fp;
		return auth;
	}
	const string token = tokens->Token();
	if (interactive) {
		PersistRotatedRefreshToken(opener, match, credential_fp, refresh_token, lineage);
	}

	AuthContext auth;
	auth.tokens = tokens;
	auth.graph_endpoint = graph_endpoint;
	auth.drive_id = ResolveDriveId(secret, graph_endpoint, token, credential_fp, require_drive);
	auth.credential_fp = credential_fp;
	// Keys every per-identity cache downstream (block cache) and the
	// same-drive check on moves: credentials + cloud + drive.
	auth.identity = Fingerprint({credential_fp, graph_endpoint, auth.drive_id});
	return auth;
}

} // namespace

AuthContext GetAuthContext(optional_ptr<FileOpener> opener, const string &path, bool require_drive) {
	return BuildAuthContext(opener, path, require_drive, string(), nullptr);
}

shared_ptr<TokenSource> GetSharepointRestTokens(optional_ptr<FileOpener> opener, const string &path,
                                                const string &host, bool &client_secret_app) {
	// A token goes only to a SharePoint host, never to one a caller made up.
	if (host.empty() || !IsSharepointWebUrl("https://" + host + "/")) {
		throw InvalidInputException("sharepoint: '%s' is not a SharePoint host", host);
	}
	return BuildAuthContext(opener, path, false, StringUtil::Lower(host), &client_secret_app).tokens;
}

ResolvedPath ResolveSharepointPath(optional_ptr<FileOpener> opener, const string &path) {
	ResolvedPath out;
	if (!IsSharepointWebUrl(path)) {
		out.auth = GetAuthContext(opener, path);
		out.path = path;
		return out;
	}
	const auto parsed = ParseSharepointWebUrl(path);
	if (!parsed.ok) {
		throw InvalidInputException("sharepoint: '%s': %s", path, parsed.error);
	}
	const auto &url = parsed.url;
	// The secret applies as for any path (its SCOPE may name the site's URL);
	// its own drive is not needed -- the URL names one.
	out.auth = GetAuthContext(opener, path, false);
	const auto &auth = out.auth;
	const string token = auth.tokens->Token();
	string drive_id;
	if (url.kind == WebUrlKind::SHARING) {
		const string key = Fingerprint({auth.credential_fp, auth.graph_endpoint, url.share_url});
		string found;
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_shared_items.find(key);
			if (it != g_shared_items.end()) {
				found = it->second;
			}
		}
		if (found.empty()) {
			const string what = "the sharing link '" + path + "'";
			auto item = GraphGetObject(auth.graph_endpoint, token,
			                           "/v1.0/shares/" + SharesId(url.share_url) + "/driveItem?$select=id,parentReference",
			                           what, what + " leads to nothing these credentials can open");
			string parent_drive;
			auto parent = item.find("parentReference");
			if (parent != item.end() && parent->second.is<picojson::object>()) {
				parent_drive = JsonString(parent->second.get<picojson::object>(), "driveId");
			}
			const auto item_id = JsonString(item, "id");
			if (parent_drive.empty() || item_id.empty()) {
				throw IOException("sharepoint: %s did not say which file it is", what);
			}
			found = parent_drive + "|" + item_id;
			lock_guard<mutex> guard(g_token_lock);
			g_shared_items[key] = found;
		}
		const auto bar = found.find('|');
		drive_id = found.substr(0, bar);
		out.path = string(SHAREPOINT_SCHEME) + "id:" + found.substr(bar + 1);
	} else {
		const string key = Fingerprint({auth.credential_fp, auth.graph_endpoint, url.host, StringUtil::Lower(url.site_path)});
		vector<pair<string, string>> libraries;
		bool cached = false;
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_site_libraries.find(key);
			if (it != g_site_libraries.end()) {
				libraries = it->second;
				cached = true;
			}
		}
		const string site_url = "https://" + url.host + (url.site_path.empty() ? "" : "/" + url.site_path);
		if (!cached) {
			const auto site_id = SiteIdOfUrl(url, site_url, auth.graph_endpoint, token);
			auto drives = GraphGetObject(auth.graph_endpoint, token,
			                             "/v1.0/sites/" + site_id + "/drives?$select=id,webUrl",
			                             "the document libraries of '" + site_url + "'", string());
			auto values = drives.find("value");
			if (values != drives.end() && values->second.is<picojson::array>()) {
				for (auto &d : values->second.get<picojson::array>()) {
					if (d.is<picojson::object>()) {
						const auto &o = d.get<picojson::object>();
						libraries.emplace_back(JsonString(o, "id"), JsonString(o, "webUrl"));
					}
				}
			}
			lock_guard<mutex> guard(g_token_lock);
			g_site_libraries[key] = libraries;
		}
		int covered = -1;
		for (auto &library : libraries) {
			const int n = MatchDriveWebUrl(url, library.second);
			if (n > covered) {
				covered = n;
				drive_id = library.first;
				out.web_prefix = library.second;
			}
		}
		if (covered < 0) {
			throw InvalidInputException("sharepoint: '%s' is in no document library of '%s' -- copy the address of a "
			                            "file or folder in a library (SELECT * FROM sharepoint_drives('%s') lists them)",
			                            path, site_url, site_url);
		}
		while (!out.web_prefix.empty() && out.web_prefix.back() == '/') {
			out.web_prefix.pop_back();
		}
		out.path = SHAREPOINT_SCHEME;
		for (std::size_t i = static_cast<std::size_t>(covered); i < url.rest.size(); i++) {
			out.path += (i > static_cast<std::size_t>(covered) ? "/" : "") + url.rest[i];
		}
		if (!url.version.empty()) {
			out.path += "?version=" + url.version;
		}
	}
	out.auth.drive_id = drive_id;
	// The same formula as GetAuthContext's: the file has one identity whether
	// it is reached by sharepoint:// or by its browser address.
	out.auth.identity = Fingerprint({auth.credential_fp, auth.graph_endpoint, drive_id});
	return out;
}

string WebPathOf(const ResolvedPath &target, const string &root_relative) {
	if (target.web_prefix.empty()) {
		return string(SHAREPOINT_SCHEME) + root_relative;
	}
	string out = target.web_prefix;
	for (auto &segment : StringUtil::Split(root_relative, '/')) {
		out += "/" + PercentEncodeSegment(segment);
	}
	return out;
}

bool HasAnySharepointSecret(optional_ptr<FileOpener> opener) {
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!secret_manager || !transaction) {
		return false;
	}
	for (const auto &entry : secret_manager->AllSecrets(*transaction)) {
		if (entry.secret && entry.secret->GetType() == kSecretTypeName) {
			return true;
		}
	}
	return false;
}

} // namespace sharepoint
} // namespace duckdb
