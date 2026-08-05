// Token acquisition + caching for the `sharepoint` secret.
//
// STATUS: this file is part of the network frontier of the scaffold. The DuckDB
// secret-lookup and httplib call shapes are written against DuckDB v1.x and
// Microsoft Graph as documented, but the extension has not yet been compiled or
// run against a live tenant (there is no DuckDB submodule checked out). Treat
// every Graph interaction here as reviewed-but-unverified until the live SQL
// suite (test/sql/live/, `make test_live`) passes. See docs/hld.md.
#include "sharepoint_auth.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <picojson.h>

#include <chrono>

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
unordered_map<string, CachedToken> g_tokens; // identity -> token

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
	cli.set_follow_location(true);
	cli.set_connection_timeout(30);
	cli.set_read_timeout(120);
	return cli;
}

// POST an application/x-www-form-urlencoded body to the Azure AD token
// endpoint and return the parsed JSON, throwing a classified DuckDB exception
// on any non-2xx.
picojson::object PostToken(const string &login_endpoint, const string &tenant,
                           const duckdb_httplib_openssl::Params &params) {
	auto cli = MakeClient(login_endpoint);
	string path = "/" + tenant + "/oauth2/v2.0/token";
	auto res = cli.Post(path.c_str(), params);
	if (!res) {
		throw IOException("sharepoint: token request to %s%s failed at the transport layer",
		                  login_endpoint.c_str(), path.c_str());
	}
	if (res->status < 200 || res->status >= 300) {
		// Azure AD returns { "error": "...", "error_description": "..." }.
		// The description can leak configuration detail but never the secret;
		// still, keep it short.
		throw IOException("sharepoint: token endpoint returned HTTP %d (%s). Check TENANT_ID, CLIENT_ID and "
		                  "CLIENT_SECRET, and that the app has admin-consented application permissions.",
		                  res->status, GraphErrorKindName(ClassifyGraphError(res->status, "")));
	}
	picojson::value v;
	string err = picojson::parse(v, res->body);
	if (!err.empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: could not parse the token endpoint response as JSON");
	}
	return v.get<picojson::object>();
}

// The `sharepoint` secret in scope for `path`, or throw a teaching error.
const KeyValueSecret &LookupSecret(ClientContext &context, const string &path, string &out_secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, path, kSecretTypeName);
	if (!match.HasMatch()) {
		throw InvalidInputException(
		    "sharepoint: no `sharepoint` secret is configured. Create one, e.g.\n"
		    "  CREATE SECRET sp (TYPE sharepoint, PROVIDER client_credentials,\n"
		    "                    TENANT_ID '...', CLIENT_ID '...', CLIENT_SECRET '...', DRIVE_ID 'b!...');");
	}
	const auto &base = match.GetSecret();
	out_secret_name = base.GetName();
	return dynamic_cast<const KeyValueSecret &>(base);
}

// Resolve the drive the paths hang off. Prefer an explicit DRIVE_ID; else
// derive it from SITE_ID via GET /sites/{site-id}/drive (the site's default
// document library). Result is not cached separately here -- it rides the
// AuthContext, rebuilt per query, which is cheap next to the data calls.
string ResolveDriveId(const KeyValueSecret &secret, const string &graph_endpoint, const string &token) {
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
	return id.get<string>();
}

} // namespace

void InvalidateAuthCache(const string &secret_name) {
	lock_guard<mutex> guard(g_token_lock);
	for (auto it = g_tokens.begin(); it != g_tokens.end();) {
		// identity is "<secret_name>|<drive>"; drop every entry for the secret.
		if (it->first.rfind(secret_name + "|", 0) == 0) {
			it = g_tokens.erase(it);
		} else {
			++it;
		}
	}
}

AuthContext GetAuthContext(ClientContext &context, const string &path) {
	string secret_name;
	const auto &secret = LookupSecret(context, path, secret_name);

	string login_endpoint = "https://login.microsoftonline.com";
	string graph_endpoint = "https://graph.microsoft.com";
	{
		Value v;
		if (context.TryGetCurrentSetting("sharepoint_login_endpoint", v) && !v.IsNull()) {
			login_endpoint = v.ToString();
		}
		if (context.TryGetCurrentSetting("sharepoint_graph_endpoint", v) && !v.IsNull()) {
			graph_endpoint = v.ToString();
		}
	}

	const string provider = secret.GetProvider();
	const string tenant = SecretField(secret, "tenant_id");
	const string scope = SecretField(secret, "graph_scope");

	// A config secret with a bare ACCESS_TOKEN is used as-is, no caching or
	// refresh -- the caller owns its lifetime.
	string static_token = (provider == "config") ? SecretField(secret, "access_token") : string();

	// Identity keys the token cache AND everything downstream. Include the
	// drive so two secrets pointing at different drives never share a token.
	string drive_hint = SecretField(secret, "drive_id");
	string identity = secret_name + "|" + drive_hint;

	string token;
	if (!static_token.empty()) {
		token = static_token;
	} else {
		// Cached?
		{
			lock_guard<mutex> guard(g_token_lock);
			auto it = g_tokens.find(identity);
			if (it != g_tokens.end() &&
			    it->second.expires_at > std::chrono::system_clock::now() + std::chrono::seconds(60)) {
				token = it->second.access_token;
			}
		}
		if (token.empty()) {
			duckdb_httplib_openssl::Params params;
			params.emplace("client_id", SecretField(secret, "client_id"));
			params.emplace("client_secret", SecretField(secret, "client_secret"));
			if (provider == "config") {
				// refresh_token grant (config secret without a static token).
				params.emplace("grant_type", "refresh_token");
				params.emplace("refresh_token", SecretField(secret, "refresh_token"));
				params.emplace("scope", scope.empty() ? SCOPE_GRAPH_DEFAULT : scope);
			} else {
				// client_credentials (the default provider).
				params.emplace("grant_type", "client_credentials");
				params.emplace("scope", scope.empty() ? SCOPE_GRAPH_DEFAULT : scope);
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
			}
			lock_guard<mutex> guard(g_token_lock);
			g_tokens[identity] = CachedToken{
			    token, std::chrono::system_clock::now() + std::chrono::seconds(expires_in)};
		}
	}

	AuthContext auth;
	auth.access_token = token;
	auth.graph_endpoint = graph_endpoint;
	auth.drive_id = ResolveDriveId(secret, graph_endpoint, token);
	auth.identity = secret_name + "|" + auth.drive_id;
	return auth;
}

} // namespace sharepoint
} // namespace duckdb
