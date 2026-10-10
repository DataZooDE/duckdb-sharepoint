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
#include "sharepoint_counters.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_identity.hpp"
#include "sharepoint_secret.hpp"

#include "duckdb/common/exception.hpp"
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
#include <cstdint>

namespace duckdb {
namespace sharepoint {

namespace {

// Cached bearer token, valid until `expires_at`. Keyed by the secret's
// identity so one secret's token is never served to another.
struct CachedToken {
	string access_token;
	std::chrono::system_clock::time_point expires_at;
};

mutex g_token_lock;
unordered_map<string, CachedToken> g_tokens;  // credential fingerprint -> token
unordered_map<string, string> g_site_drives; // fingerprint of (credentials, site) -> drive id

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
picojson::object PostToken(const string &login_endpoint, const string &tenant,
                           const duckdb_httplib_openssl::Params &params) {
	IncrementCounter(Counter::TOKEN_REQUEST);
	auto cli = MakeClient(login_endpoint);
	string path = "/" + tenant + "/oauth2/v2.0/token";
	auto res = cli.Post(path.c_str(), params);
	if (!res) {
		throw IOException("sharepoint: token request to %s%s failed at the transport layer",
		                  login_endpoint.c_str(), path.c_str());
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
		    "                    TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...', DRIVE_ID 'b!...');",
		    path);
	}
	return match;
}

// Resolve the drive the paths hang off. Prefer an explicit DRIVE_ID; else
// derive it from SITE_ID via GET /sites/{site-id}/drive (the site's default
// document library), cached per credentials + site.
string ResolveDriveId(const KeyValueSecret &secret, const string &graph_endpoint, const string &token,
                      const string &credential_fp) {
	string drive_id = SecretField(secret, "drive_id");
	if (!drive_id.empty()) {
		return drive_id;
	}
	string site_id = SecretField(secret, "site_id");
	if (site_id.empty()) {
		throw InvalidInputException(
		    "sharepoint: the secret gives neither DRIVE_ID nor SITE_ID, so there is no drive to address. "
		    "Pass one of them to CREATE SECRET.");
	}
	// A site's default library does not move; resolve it once per (credentials,
	// cloud, site) instead of one extra Graph call per file operation.
	const string cache_key = Fingerprint({credential_fp, graph_endpoint, site_id});
	{
		lock_guard<mutex> guard(g_token_lock);
		auto it = g_site_drives.find(cache_key);
		if (it != g_site_drives.end()) {
			return it->second;
		}
	}
	auto cli = MakeClient(graph_endpoint);
	duckdb_httplib_openssl::Headers headers = {{"Authorization", "Bearer " + token}};
	string path = "/v1.0/sites/" + site_id + "/drive?$select=id";
	auto res = cli.Get(path.c_str(), headers);
	if (!res || res->status < 200 || res->status >= 300) {
		throw IOException("sharepoint: could not resolve the default document library for SITE_ID '%s' (HTTP "
		                  "%d)",
		                  site_id.c_str(), res ? res->status : 0);
	}
	picojson::value v;
	if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected response resolving the site drive");
	}
	auto id = v.get<picojson::object>()["id"];
	if (!id.is<string>()) {
		throw IOException("sharepoint: site drive response carried no id");
	}
	lock_guard<mutex> guard(g_token_lock);
	g_site_drives[cache_key] = id.get<string>();
	return id.get<string>();
}

} // namespace

AuthContext GetAuthContext(optional_ptr<FileOpener> opener, const string &path) {
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

	const string provider = secret.GetProvider();
	const string tenant = SecretField(secret, "tenant_id");
	const string client_id = SecretField(secret, "client_id");
	const string client_secret = SecretField(secret, "client_secret");
	const string refresh_token = SecretField(secret, "refresh_token");
	const string configured_scope = SecretField(secret, "graph_scope");
	const string scope = configured_scope.empty() ? DefaultGraphScope(graph_endpoint) : configured_scope;

	// A config secret with a bare ACCESS_TOKEN is used as-is, no caching or
	// refresh -- the caller owns its lifetime.
	const string static_token = (provider == "config") ? SecretField(secret, "access_token") : string();

	// Everything that changes which token comes back -- and where it may be
	// sent. Two secrets (or one name recreated) share a token only if all of
	// this is identical.
	const string credential_fp = Fingerprint({provider, tenant, client_id, client_secret, refresh_token, scope,
	                                          login_endpoint, static_token});

	string token;
	if (!static_token.empty()) {
		token = static_token;
	} else {
		if (!IsValidTenantId(tenant)) {
			throw InvalidInputException("sharepoint: TENANT_ID must be a tenant GUID or domain (letters, digits, "
			                            "'-', '.')");
		}
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(credential_fp);
			if (it != g_tokens.end() &&
			    it->second.expires_at > std::chrono::system_clock::now() + std::chrono::seconds(60)) {
				token = it->second.access_token;
			}
		}
		if (token.empty()) {
			duckdb_httplib_openssl::Params params;
			params.emplace("client_id", client_id);
			params.emplace("client_secret", client_secret);
			params.emplace("scope", scope);
			if (provider == "config") {
				// refresh_token grant (config secret without a static token).
				params.emplace("grant_type", "refresh_token");
				params.emplace("refresh_token", refresh_token);
			} else {
				// client_credentials (the default provider).
				params.emplace("grant_type", "client_credentials");
			}
			auto obj = PostToken(login_endpoint, tenant, params);
			auto at = obj.find("access_token");
			if (at == obj.end() || !at->second.is<string>()) {
				throw IOException("sharepoint: token endpoint returned no access_token");
			}
			token = at->second.get<string>();
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
			lock_guard<mutex> guard(g_token_lock);
			g_tokens[credential_fp] =
			    CachedToken {token, std::chrono::system_clock::now() + std::chrono::seconds(expires_in)};
		}
	}

	AuthContext auth;
	auth.access_token = token;
	auth.graph_endpoint = graph_endpoint;
	auth.drive_id = ResolveDriveId(secret, graph_endpoint, token, credential_fp);
	// Keys every per-identity cache downstream (block cache) and the
	// same-drive check on moves: credentials + cloud + drive.
	auth.identity = Fingerprint({credential_fp, graph_endpoint, auth.drive_id});
	return auth;
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
