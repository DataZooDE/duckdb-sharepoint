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
		erpl_web::OAuth2Server server(cfg.redirect_port);
		code = server.StartAndWaitForCode(state, cfg.redirect_port);
	} catch (const std::exception &e) {
		throw IOException("sharepoint secret '%s' (PROVIDER authorization_code): the browser sign-in did not "
		                  "complete: %s. It needs a free localhost port (%d; set REDIRECT_PORT) and a browser on "
		                  "this machine; on a headless host use PROVIDER device_code.",
		                  cfg.secret_name, e.what(), cfg.redirect_port);
	}

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

void PersistRefreshToken(ClientContext &context, const SecretMatch &match, const string &refresh_token) {
	// Through the secret manager: the KeyValueSecret a lookup returns is a deep
	// copy, so writing to it would change nothing (the trap gdrive documents).
	const auto &current = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
	auto updated = make_uniq<KeyValueSecret>(current);
	updated->secret_map["refresh_token"] = Value(refresh_token);
	updated->redact_keys.insert("refresh_token");
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	secret_manager.RegisterSecret(transaction, std::move(updated), OnCreateConflict::REPLACE_ON_CONFLICT,
	                              match.secret_entry->persist_type, match.secret_entry->storage_mode);
}

} // namespace sharepoint
} // namespace duckdb
