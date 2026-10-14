// Delegated (user) sign-in: device code (RFC 8628) and authorization code +
// PKCE. Proven live (test/sql/sharepoint.live.template drives a real
// device-code request; sharepoint_delegated.live.template and e2e/ cover the
// delegated token end to end).
#include "sharepoint_delegated_flow.hpp"
#include "sharepoint_counters.hpp"
#include "sharepoint_delegated.hpp"
#include "sharepoint_preauth.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

#ifndef EMSCRIPTEN
// datazoo-oauth2 -- the shared OAuth2 stack the sibling gdrive uses: PKCE,
// the loopback redirect server and the browser launcher.
#include "datazoo/oauth2/oauth2_browser.hpp"
#include "datazoo/oauth2/oauth2_server.hpp"
#include "datazoo/oauth2/oauth2_types.hpp"
#include "datazoo/oauth2/oauth2_url_pure.hpp"
#endif

#include <chrono>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace duckdb {
namespace sharepoint {

namespace {

//! Token-endpoint replies are small JSON documents; anything bigger is wrong.
constexpr std::size_t kMaxTokenReplyBytes = 1 << 20;
constexpr int kTokenRequestTimeoutSeconds = 60;

//! POST a form to an Entra ID endpoint over the raw TLS transport. Returns the
//! status and body for ANY HTTP status; transport failures throw IOException.
RawHttpResponse PostForm(const string &url, const FormParams &params) {
	IncrementCounter(Counter::TOKEN_REQUEST);
	const RawHeaders headers = {{"Content-Type", "application/x-www-form-urlencoded"},
	                            {"Accept", "application/json"}};
	return PreauthenticatedRequest("POST", url, headers, FormUrlEncode(params), kTokenRequestTimeoutSeconds,
	                               kMaxTokenReplyBytes);
}

DelegatedTokens ToTokens(const TokenReply &reply) {
	DelegatedTokens t;
	t.access_token = reply.access_token;
	t.refresh_token = reply.refresh_token;
	t.expires_in = reply.expires_in > 0 ? reply.expires_in : 3600;
	return t;
}

string GetEnvOrEmpty(const char *name) {
	const char *v = std::getenv(name);
	return v ? string(v) : string();
}

} // namespace

DelegatedTokens RunDeviceCodeFlow(const DelegatedFlowConfig &cfg, const std::function<bool()> &interrupted) {
	const auto res =
	    PostForm(EntraUrl(cfg.login_endpoint, cfg.tenant, "devicecode"), DeviceCodeRequest(cfg.client_id, cfg.scope));
	if (res.status != 200) {
		throw IOException(DelegatedErrorMessage(ParseTokenReply(res.status, res.body), cfg.secret_name, cfg.provider));
	}
	DeviceCodeGrant grant;
	string parse_error;
	if (!ParseDeviceCodeResponse(res.body, grant, parse_error)) {
		throw IOException("sharepoint secret '%s' (PROVIDER %s): %s", cfg.secret_name, cfg.provider, parse_error);
	}

	// The ONLY output: where to go and what to type. Never the device code.
	Printer::Print(OutputStream::STREAM_STDERR, DeviceCodePrompt(grant));
	Printer::Flush(OutputStream::STREAM_STDERR);

	using Clock = std::chrono::steady_clock;
	const int64_t budget = std::min<int64_t>(grant.expires_in, std::max<int64_t>(cfg.timeout_seconds, 1));
	const auto deadline = Clock::now() + std::chrono::seconds(budget);
	int64_t interval = InitialPollIntervalSeconds(grant.interval);
	const string token_url = EntraUrl(cfg.login_endpoint, cfg.tenant, "token");

	while (true) {
		// Wait one interval in short slices so an interrupt (Ctrl-C) is honoured
		// promptly, and never past the deadline.
		const auto wake = std::min(deadline, Clock::now() + std::chrono::seconds(interval));
		while (Clock::now() < wake) {
			if (interrupted && interrupted()) {
				throw InterruptException();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		if (Clock::now() >= deadline) {
			throw IOException("sharepoint secret '%s' (PROVIDER %s): the device-code sign-in was not completed "
			                  "within %lld seconds. Run the query again for a new code (the wait is set by "
			                  "sharepoint_device_code_timeout_seconds).",
			                  cfg.secret_name, cfg.provider, (long long)budget);
		}
		const auto poll = PostForm(token_url, DeviceCodeTokenRequest(cfg.client_id, grant.device_code));
		const TokenReply reply = ParseTokenReply(poll.status, poll.body);
		switch (reply.kind) {
		case TokenReplyKind::SUCCESS:
			return ToTokens(reply);
		case TokenReplyKind::AUTHORIZATION_PENDING:
		case TokenReplyKind::SLOW_DOWN:
		case TokenReplyKind::TRANSIENT:
			interval = NextPollIntervalSeconds(interval, reply.kind);
			continue;
		default:
			throw IOException(DelegatedErrorMessage(reply, cfg.secret_name, cfg.provider));
		}
	}
}

DelegatedTokens RunAuthorizationCodeFlow(const DelegatedFlowConfig &cfg) {
#ifdef EMSCRIPTEN
	throw NotImplementedException("sharepoint: PROVIDER authorization_code is not available in this build");
#else
	// Fail fast: on a headless host the browser launcher would do nothing and
	// the loopback server would time out, which reads as "you were too slow".
	bool posix_non_apple = false;
#if !defined(_WIN32) && !defined(__APPLE__)
	posix_non_apple = true;
#endif
	if (!CanLaunchBrowser(posix_non_apple, GetEnvOrEmpty("DISPLAY"), GetEnvOrEmpty("WAYLAND_DISPLAY"))) {
		throw IOException("sharepoint secret '%s' (PROVIDER authorization_code): no browser is available on this "
		                  "machine (neither DISPLAY nor WAYLAND_DISPLAY is set). Use PROVIDER device_code instead: "
		                  "it prints a URL and a code to enter on any other device.",
		                  cfg.secret_name);
	}

	// PKCE S256 (RFC 7636) and a CSRF state, from datazoo-oauth2.
	const std::string verifier = erpl_web::OAuth2Utils::GenerateCodeVerifier();
	const std::string challenge = erpl_web::OAuth2Utils::GenerateCodeChallengeS256(verifier);
	const std::string state = erpl_web::OAuth2Utils::GenerateState();

	erpl_web::OAuth2Config config;
	config.custom_client = true; // a public client: never the SAP pre-delivered shape
	config.client_id = cfg.client_id;
	config.scope = cfg.scope;
	// Entra matches any port on an http://localhost redirect registered under
	// "Mobile and desktop applications".
	config.redirect_uri = "http://localhost:" + std::to_string(cfg.redirect_port);
	config.custom_auth_url = EntraUrl(cfg.login_endpoint, cfg.tenant, "authorize");
	config.custom_token_url = EntraUrl(cfg.login_endpoint, cfg.tenant, "token");
	const std::string auth_url = erpl_web::BuildAuthorizationUrlPure(config, challenge, state);

	// Listen BEFORE the browser opens: an existing browser session can redirect
	// back within milliseconds, and a redirect to a closed port is lost.
	erpl_web::OAuth2Server server(cfg.redirect_port);
	try {
		server.Start(state, cfg.redirect_port);
	} catch (const std::exception &e) {
		throw IOException("sharepoint secret '%s' (PROVIDER authorization_code): %s. Set REDIRECT_PORT to a free "
		                  "port.",
		                  cfg.secret_name, e.what());
	}

	Printer::Print(OutputStream::STREAM_STDERR,
	               "sharepoint: opening a browser to sign in. If it does not open, visit: " + auth_url);
	Printer::Flush(OutputStream::STREAM_STDERR);
	try {
		erpl_web::OAuth2Browser::OpenUrl(auth_url);
	} catch (const std::exception &) {
		// The URL is printed above; the user can open it by hand.
	}

	std::string code;
	try {
		code = server.WaitForCode();
	} catch (const std::exception &e) {
		throw IOException("sharepoint secret '%s' (PROVIDER authorization_code): the browser sign-in did not "
		                  "complete: %s. It needs a browser on this machine; on a headless host use PROVIDER "
		                  "device_code.",
		                  cfg.secret_name, e.what());
	}
	server.Stop();

	const auto res = PostForm(config.custom_token_url,
	                          AuthorizationCodeTokenRequest(cfg.client_id, code, config.redirect_uri, verifier,
	                                                        cfg.scope));
	const TokenReply reply = ParseTokenReply(res.status, res.body);
	if (reply.kind != TokenReplyKind::SUCCESS) {
		throw IOException(DelegatedErrorMessage(reply, cfg.secret_name, cfg.provider));
	}
	return ToTokens(reply);
#endif
}

namespace {

string Field(const KeyValueSecret &kv, const char *key) {
	auto it = kv.secret_map.find(key);
	return (it == kv.secret_map.end() || it->second.IsNull()) ? string() : it->second.ToString();
}

//! Same secret configuration, ignoring the fields a write-back changes.
bool SameConfiguration(const KeyValueSecret &a, const KeyValueSecret &b) {
	if (a.GetType() != b.GetType() || a.GetProvider() != b.GetProvider() || a.GetName() != b.GetName() ||
	    a.GetScope() != b.GetScope()) {
		return false;
	}
	auto stripped = [](const KeyValueSecret &kv) {
		std::map<string, string> out;
		for (auto &entry : kv.secret_map) {
			if (entry.first == "refresh_token" || entry.first == "refresh_token_lineage") {
				continue;
			}
			out[entry.first] = entry.second.IsNull() ? string("\x01NULL") : entry.second.ToString();
		}
		return out;
	};
	return stripped(a) == stripped(b);
}

//! The current registered secret, if it still has the snapshot's configuration.
unique_ptr<SecretEntry> RefetchMatching(ClientContext &context, const SecretMatch &snapshot) {
	const auto &before = dynamic_cast<const KeyValueSecret &>(snapshot.GetSecret());
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = secret_manager.GetSecretByName(transaction, before.GetName(), snapshot.secret_entry->storage_mode);
	if (!entry || !entry->secret) {
		return nullptr;
	}
	auto now = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
	if (!now || !SameConfiguration(before, *now)) {
		return nullptr;
	}
	return entry;
}

} // namespace

StoredDelegatedState RefetchDelegatedState(ClientContext &context, const SecretMatch &snapshot) {
	StoredDelegatedState state;
	auto entry = RefetchMatching(context, snapshot);
	if (entry) {
		const auto &kv = dynamic_cast<const KeyValueSecret &>(*entry->secret);
		state.present = true;
		state.refresh_token = Field(kv, "refresh_token");
		state.lineage = Field(kv, "refresh_token_lineage");
	}
	return state;
}

bool WriteBackRefreshToken(ClientContext &context, const SecretMatch &snapshot, const string &expected_current,
                           const string &refresh_token, const string &lineage) {
	// Re-fetched rather than written from the snapshot: a sign-in can take
	// minutes, and a CREATE OR REPLACE / DROP of this secret meanwhile must not
	// be silently reverted or resurrected. (Writing to the snapshot itself would
	// change nothing either: a lookup returns a deep copy -- gdrive's trap.)
	auto entry = RefetchMatching(context, snapshot);
	if (!entry) {
		return false;
	}
	const auto &current = dynamic_cast<const KeyValueSecret &>(*entry->secret);
	if (Field(current, "refresh_token") != expected_current) {
		return false; // someone else already updated it
	}
	auto updated = make_uniq<KeyValueSecret>(current);
	updated->secret_map["refresh_token"] = Value(refresh_token);
	updated->secret_map["refresh_token_lineage"] = Value(lineage);
	updated->redact_keys.insert("refresh_token");
	updated->redact_keys.insert("refresh_token_lineage");
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	secret_manager.RegisterSecret(transaction, std::move(updated), OnCreateConflict::REPLACE_ON_CONFLICT,
	                              entry->persist_type, entry->storage_mode);
	return true;
}

} // namespace sharepoint
} // namespace duckdb
