// Catch2 pure-logic tests for delegated (user) auth: request construction,
// token-endpoint reply classification and the RFC 8628 polling rules. The
// reply bodies are the shapes Entra ID returned live (2026-10-14).
#include "sharepoint_delegated.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace duckdb::sharepoint;

namespace {

std::string Get(const FormParams &p, const std::string &key) {
	for (const auto &kv : p) {
		if (kv.first == key) {
			return kv.second;
		}
	}
	return "<absent>";
}

bool Has(const FormParams &p, const std::string &key) {
	return Get(p, key) != "<absent>";
}

} // namespace

TEST_CASE("form encoding escapes everything but unreserved bytes", "[delegated]") {
	REQUIRE(FormUrlEncode({}) == "");
	REQUIRE(FormUrlEncode({{"a", "b"}}) == "a=b");
	REQUIRE(FormUrlEncode({{"scope", "https://graph.microsoft.com/Files.ReadWrite.All offline_access"},
	                       {"x", "a&b=c+d~_.-"}}) ==
	        "scope=https%3A%2F%2Fgraph.microsoft.com%2FFiles.ReadWrite.All%20offline_access&x=a%26b%3Dc%2Bd~_.-");
	// Order is preserved, never sorted.
	REQUIRE(FormUrlEncode({{"z", "1"}, {"a", "2"}}) == "z=1&a=2");
	// Non-ASCII is percent-encoded byte by byte.
	REQUIRE(FormUrlEncode({{"k", "\xC3\xA4"}}) == "k=%C3%A4");
}

TEST_CASE("the delegated scope follows the Graph cloud and asks for a refresh token", "[delegated]") {
	REQUIRE(DelegatedGraphScope("https://graph.microsoft.com") ==
	        "https://graph.microsoft.com/Files.ReadWrite.All https://graph.microsoft.com/Sites.ReadWrite.All "
	        "offline_access");
	REQUIRE(DelegatedGraphScope("https://graph.microsoft.us/") ==
	        "https://graph.microsoft.us/Files.ReadWrite.All https://graph.microsoft.us/Sites.ReadWrite.All "
	        "offline_access");
}

TEST_CASE("entra v2 endpoint URLs", "[delegated]") {
	REQUIRE(EntraUrl("https://login.microsoftonline.com", "contoso.onmicrosoft.com", "devicecode") ==
	        "https://login.microsoftonline.com/contoso.onmicrosoft.com/oauth2/v2.0/devicecode");
	REQUIRE(EntraUrl("https://login.microsoftonline.us/", "t", "token") ==
	        "https://login.microsoftonline.us/t/oauth2/v2.0/token");
	REQUIRE(EntraUrl("https://login.chinacloudapi.cn//", "t", "authorize") ==
	        "https://login.chinacloudapi.cn/t/oauth2/v2.0/authorize");
}

TEST_CASE("device code requests are public-client requests", "[delegated]") {
	auto dc = DeviceCodeRequest("cid", "s1 s2");
	REQUIRE(Get(dc, "client_id") == "cid");
	REQUIRE(Get(dc, "scope") == "s1 s2");
	REQUIRE_FALSE(Has(dc, "client_secret"));

	auto poll = DeviceCodeTokenRequest("cid", "DEV");
	REQUIRE(Get(poll, "grant_type") == "urn:ietf:params:oauth:grant-type:device_code");
	REQUIRE(Get(poll, "client_id") == "cid");
	REQUIRE(Get(poll, "device_code") == "DEV");
	REQUIRE_FALSE(Has(poll, "client_secret"));
}

TEST_CASE("authorization code redemption carries the PKCE verifier and redirect", "[delegated]") {
	auto p = AuthorizationCodeTokenRequest("cid", "CODE", "http://localhost:8020", "VERIFIER", "s");
	REQUIRE(Get(p, "grant_type") == "authorization_code");
	REQUIRE(Get(p, "code") == "CODE");
	REQUIRE(Get(p, "redirect_uri") == "http://localhost:8020");
	REQUIRE(Get(p, "code_verifier") == "VERIFIER");
	REQUIRE(Get(p, "client_id") == "cid");
	REQUIRE(Get(p, "scope") == "s");
	REQUIRE_FALSE(Has(p, "client_secret"));
}

TEST_CASE("refresh requests send a client secret only for confidential clients", "[delegated]") {
	auto pub = RefreshTokenRequest("cid", "", "RT", "s");
	REQUIRE(Get(pub, "grant_type") == "refresh_token");
	REQUIRE(Get(pub, "refresh_token") == "RT");
	REQUIRE(Get(pub, "scope") == "s");
	REQUIRE_FALSE(Has(pub, "client_secret"));

	auto conf = RefreshTokenRequest("cid", "SEC", "RT", "s");
	REQUIRE(Get(conf, "client_secret") == "SEC");
}

TEST_CASE("device code responses parse and are validated", "[delegated]") {
	DeviceCodeGrant g;
	std::string err;
	REQUIRE(ParseDeviceCodeResponse(
	    R"({"user_code":"ABCD1234","device_code":"DAQABAAEAAAD","verification_uri":"https://login.microsoft.com/device",
	        "expires_in":900,"interval":5,"message":"To sign in, use a web browser..."})",
	    g, err));
	REQUIRE(g.user_code == "ABCD1234");
	REQUIRE(g.device_code == "DAQABAAEAAAD");
	REQUIRE(g.verification_uri == "https://login.microsoft.com/device");
	REQUIRE(g.expires_in == 900);
	REQUIRE(g.interval == 5);

	// Some Entra builds send the numbers as strings.
	REQUIRE(ParseDeviceCodeResponse(
	    R"({"user_code":"U","device_code":"D","verification_uri":"https://microsoft.com/devicelogin","expires_in":"900","interval":"5"})",
	    g, err));
	REQUIRE(g.expires_in == 900);
	REQUIRE(g.interval == 5);

	// No interval -> 0 here; the polling rule turns it into 5 s.
	REQUIRE(ParseDeviceCodeResponse(
	    R"({"user_code":"U","device_code":"D","verification_uri":"https://microsoft.com/devicelogin","expires_in":900})", g,
	    err));
	REQUIRE(g.interval == 0);

	REQUIRE_FALSE(ParseDeviceCodeResponse("not json", g, err));
	REQUIRE_FALSE(err.empty());
	REQUIRE_FALSE(ParseDeviceCodeResponse(R"({"user_code":"U","verification_uri":"https://x","expires_in":900})", g,
	                                      err)); // no device_code
	REQUIRE_FALSE(ParseDeviceCodeResponse(
	    R"({"user_code":"U","device_code":"D","verification_uri":"http://phish.example","expires_in":900})", g, err));
	REQUIRE_FALSE(ParseDeviceCodeResponse(
	    R"({"user_code":"U","device_code":"D","verification_uri":"https://x","expires_in":0})", g, err));
	// The error never echoes the device code.
	REQUIRE(err.find("\"D\"") == std::string::npos);
}

TEST_CASE("token replies classify per RFC 6749 / 8628 and Entra's AADSTS codes", "[delegated]") {
	auto ok = ParseTokenReply(200, R"({"token_type":"Bearer","scope":"x","expires_in":4567,"ext_expires_in":4567,
	                                   "access_token":"AT","refresh_token":"RT"})");
	REQUIRE(ok.kind == TokenReplyKind::SUCCESS);
	REQUIRE(ok.access_token == "AT");
	REQUIRE(ok.refresh_token == "RT");
	REQUIRE(ok.expires_in == 4567);

	auto str_exp = ParseTokenReply(200, R"({"access_token":"AT","expires_in":"3599"})");
	REQUIRE(str_exp.kind == TokenReplyKind::SUCCESS);
	REQUIRE(str_exp.expires_in == 3599);
	REQUIRE(str_exp.refresh_token.empty());

	REQUIRE(ParseTokenReply(200, R"({"token_type":"Bearer"})").kind == TokenReplyKind::OTHER);
	REQUIRE(ParseTokenReply(200, "garbage").kind == TokenReplyKind::OTHER);

	auto pending = ParseTokenReply(400, R"({"error":"authorization_pending","error_description":"AADSTS70016: ...",
	                                        "error_codes":[70016]})");
	REQUIRE(pending.kind == TokenReplyKind::AUTHORIZATION_PENDING);
	REQUIRE(pending.error == "authorization_pending");
	REQUIRE(pending.aadsts == 70016);

	REQUIRE(ParseTokenReply(400, R"({"error":"slow_down"})").kind == TokenReplyKind::SLOW_DOWN);
	REQUIRE(ParseTokenReply(400, R"({"error":"expired_token","error_codes":[70020]})").kind ==
	        TokenReplyKind::EXPIRED_TOKEN);
	REQUIRE(ParseTokenReply(400, R"({"error":"authorization_declined"})").kind == TokenReplyKind::DECLINED);
	REQUIRE(ParseTokenReply(400, R"({"error":"access_denied"})").kind == TokenReplyKind::DECLINED);

	// Consent: Entra reports it as invalid_grant or consent_required, with 65001.
	REQUIRE(ParseTokenReply(400, R"({"error":"invalid_grant","error_codes":[65001]})").kind ==
	        TokenReplyKind::CONSENT_REQUIRED);
	REQUIRE(ParseTokenReply(400, R"({"error":"consent_required"})").kind == TokenReplyKind::CONSENT_REQUIRED);
	REQUIRE(ParseTokenReply(400, R"({"error":"interaction_required","error_codes":[50076]})").kind ==
	        TokenReplyKind::INTERACTION_REQUIRED);

	// "Allow public client flows" is off.
	REQUIRE(ParseTokenReply(401, R"({"error":"invalid_client","error_codes":[7000218]})").kind ==
	        TokenReplyKind::PUBLIC_CLIENT_DISABLED);

	auto bad_rt = ParseTokenReply(400, R"({"error":"invalid_grant","error_codes":[9002313]})");
	REQUIRE(bad_rt.kind == TokenReplyKind::INVALID_GRANT);
	REQUIRE(bad_rt.aadsts == 9002313);
	REQUIRE(ParseTokenReply(400, R"({"error":"invalid_grant","error_codes":[70008]})").kind ==
	        TokenReplyKind::INVALID_GRANT);

	REQUIRE(ParseTokenReply(400, R"({"error":"invalid_client","error_codes":[700016]})").kind ==
	        TokenReplyKind::INVALID_CLIENT);
	REQUIRE(ParseTokenReply(400, R"({"error":"invalid_scope"})").kind == TokenReplyKind::INVALID_CLIENT);
	REQUIRE(ParseTokenReply(400, R"({"error":"unauthorized_client"})").kind == TokenReplyKind::INVALID_CLIENT);
	REQUIRE(ParseTokenReply(400, R"({"error":"invalid_request","error_codes":[50011]})").kind ==
	        TokenReplyKind::INVALID_CLIENT);

	REQUIRE(ParseTokenReply(429, "").kind == TokenReplyKind::TRANSIENT);
	REQUIRE(ParseTokenReply(503, "<html>").kind == TokenReplyKind::TRANSIENT);
	REQUIRE(ParseTokenReply(400, "<html>").kind == TokenReplyKind::OTHER);
	// A 2xx is never an error, whatever the body claims.
	REQUIRE(ParseTokenReply(200, R"({"error":"authorization_pending"})").kind == TokenReplyKind::OTHER);
}

TEST_CASE("RFC 8628 polling interval", "[delegated]") {
	REQUIRE(InitialPollIntervalSeconds(0) == 5);
	REQUIRE(InitialPollIntervalSeconds(-3) == 5);
	REQUIRE(InitialPollIntervalSeconds(5) == 5);
	REQUIRE(InitialPollIntervalSeconds(1) == 1);
	REQUIRE(InitialPollIntervalSeconds(100000) == 60);

	REQUIRE(NextPollIntervalSeconds(5, TokenReplyKind::AUTHORIZATION_PENDING) == 5);
	REQUIRE(NextPollIntervalSeconds(5, TokenReplyKind::SLOW_DOWN) == 10);
	REQUIRE(NextPollIntervalSeconds(10, TokenReplyKind::SLOW_DOWN) == 15);
	REQUIRE(NextPollIntervalSeconds(58, TokenReplyKind::SLOW_DOWN) == 60);
	REQUIRE(NextPollIntervalSeconds(5, TokenReplyKind::TRANSIENT) == 5);
}

TEST_CASE("the device-code prompt shows the URL and user code only", "[delegated]") {
	DeviceCodeGrant g;
	g.device_code = "SECRET-DEVICE-CODE";
	g.user_code = "ABCD-1234";
	g.verification_uri = "https://login.microsoft.com/device";
	g.expires_in = 900;
	const auto p = DeviceCodePrompt(g);
	REQUIRE(p.find("https://login.microsoft.com/device") != std::string::npos);
	REQUIRE(p.find("ABCD-1234") != std::string::npos);
	REQUIRE(p.find("SECRET-DEVICE-CODE") == std::string::npos);
	REQUIRE(p.find('\n') == std::string::npos);
}

TEST_CASE("delegated error messages teach the fix", "[delegated]") {
	auto msg = [](TokenReplyKind kind, const char *error, int64_t code) {
		TokenReply r;
		r.kind = kind;
		r.error = error;
		r.aadsts = code;
		return DelegatedErrorMessage(r, "sp_user", "device_code");
	};
	auto has = [](const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; };

	const auto consent = msg(TokenReplyKind::CONSENT_REQUIRED, "invalid_grant", 65001);
	REQUIRE(has(consent, "sp_user"));
	REQUIRE(has(consent, "device_code"));
	REQUIRE(has(consent, "consent"));
	REQUIRE(has(consent, "AADSTS65001"));

	REQUIRE(has(msg(TokenReplyKind::EXPIRED_TOKEN, "expired_token", 0), "expired"));
	REQUIRE(has(msg(TokenReplyKind::DECLINED, "authorization_declined", 0), "declined"));
	REQUIRE(has(msg(TokenReplyKind::PUBLIC_CLIENT_DISABLED, "invalid_client", 7000218), "public client flows"));
	REQUIRE(has(msg(TokenReplyKind::INVALID_GRANT, "invalid_grant", 70008), "sign in again"));
	REQUIRE(has(msg(TokenReplyKind::INTERACTION_REQUIRED, "interaction_required", 50076), "sign in again"));
	REQUIRE(has(msg(TokenReplyKind::INVALID_CLIENT, "invalid_client", 700016), "CLIENT_ID"));
	// One line, so it reads cleanly in a DuckDB error.
	REQUIRE_FALSE(has(consent, "\n"));
}

TEST_CASE("redirect port", "[delegated]") {
	int port = 0;
	REQUIRE(ParseRedirectPort("", port));
	REQUIRE(port == 8020);
	REQUIRE(ParseRedirectPort("53682", port));
	REQUIRE(port == 53682);
	REQUIRE_FALSE(ParseRedirectPort("80", port));
	REQUIRE_FALSE(ParseRedirectPort("65536", port));
	REQUIRE_FALSE(ParseRedirectPort("8020x", port));
	REQUIRE_FALSE(ParseRedirectPort("-1", port));
	REQUIRE_FALSE(ParseRedirectPort("99999999999999999999", port));
}

TEST_CASE("browser launch needs a display on non-Apple POSIX", "[delegated]") {
	REQUIRE(CanLaunchBrowser(false, "", ""));
	REQUIRE_FALSE(CanLaunchBrowser(true, "", ""));
	REQUIRE(CanLaunchBrowser(true, ":0", ""));
	REQUIRE(CanLaunchBrowser(true, "", "wayland-0"));
}
