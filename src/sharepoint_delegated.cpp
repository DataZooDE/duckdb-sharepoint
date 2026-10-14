// Delegated (user) auth -- the pure half. See sharepoint_delegated.hpp.
//
// PURE: no DuckDB, no I/O. picojson is a header-only parser.
#include "sharepoint_delegated.hpp"

#include <picojson.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace sharepoint {

namespace {

bool IsUnreserved(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
	       c == '.' || c == '~';
}

std::string Encode(const std::string &s) {
	static const char *kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		if (IsUnreserved(c)) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0xF]);
		}
	}
	return out;
}

std::string StripTrailingSlashes(std::string s) {
	while (!s.empty() && s.back() == '/') {
		s.pop_back();
	}
	return s;
}

std::string StringField(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	if (it != o.end() && it->second.is<std::string>()) {
		return it->second.get<std::string>();
	}
	return std::string();
}

//! Entra sends numbers as JSON integers, but some builds send strings.
int64_t IntField(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	if (it == o.end()) {
		return 0;
	}
	if (it->second.is<int64_t>()) {
		return it->second.get<int64_t>();
	}
	if (it->second.is<std::string>()) {
		const std::string &s = it->second.get<std::string>();
		if (s.empty() || s.size() > 18) {
			return 0;
		}
		char *end = nullptr;
		const long long v = std::strtoll(s.c_str(), &end, 10);
		return (end && *end == '\0') ? v : 0;
	}
	return 0;
}

bool ParseObject(const std::string &body, picojson::object &out) {
	picojson::value v;
	if (!picojson::parse(v, body).empty() || !v.is<picojson::object>()) {
		return false;
	}
	out = v.get<picojson::object>();
	return true;
}

int64_t ClampInterval(int64_t s) {
	return std::min<int64_t>(std::max<int64_t>(s, 1), 60);
}

} // namespace

bool IsDelegatedCredential(const std::string &provider, bool has_client_secret) {
	return provider == "device_code" || provider == "authorization_code" ||
	       (provider == "config" && !has_client_secret);
}

bool IsValidUserCode(const std::string &user_code) {
	if (user_code.empty() || user_code.size() > 16) {
		return false;
	}
	for (char c : user_code) {
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return false;
		}
	}
	return true;
}

bool IsAllowedVerificationUri(const std::string &uri) {
	static const std::string kScheme = "https://";
	if (uri.compare(0, kScheme.size(), kScheme) != 0 || uri.size() > 512) {
		return false;
	}
	for (unsigned char c : uri) {
		if (c <= 0x20 || c >= 0x7F) {
			return false; // whitespace, control (incl. ESC), non-ASCII
		}
	}
	const auto host_end = uri.find_first_of("/?#", kScheme.size());
	std::string host = uri.substr(kScheme.size(), host_end == std::string::npos ? std::string::npos
	                                                                             : host_end - kScheme.size());
	if (host.empty() || host.find_first_of("@:[]") != std::string::npos) {
		return false; // userinfo, port, IPv6 literal
	}
	for (auto &c : host) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	for (const char *domain :
	     {"microsoft.com", "microsoftonline.com", "microsoftonline.us", "microsoftonline.cn", "chinacloudapi.cn"}) {
		const std::string d(domain);
		if (host == d ||
		    (host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0 &&
		     host[host.size() - d.size() - 1] == '.')) {
			return true;
		}
	}
	return false;
}

std::string FormUrlEncode(const FormParams &params) {
	std::string out;
	for (const auto &kv : params) {
		if (!out.empty()) {
			out.push_back('&');
		}
		out += Encode(kv.first);
		out.push_back('=');
		out += Encode(kv.second);
	}
	return out;
}

std::string DelegatedGraphScope(const std::string &graph_endpoint) {
	const std::string g = StripTrailingSlashes(graph_endpoint);
	return g + "/Files.ReadWrite.All " + g + "/Sites.ReadWrite.All offline_access";
}

std::string EntraUrl(const std::string &login_endpoint, const std::string &tenant, const std::string &leaf) {
	return StripTrailingSlashes(login_endpoint) + "/" + tenant + "/oauth2/v2.0/" + leaf;
}

FormParams DeviceCodeRequest(const std::string &client_id, const std::string &scope) {
	return {{"client_id", client_id}, {"scope", scope}};
}

FormParams DeviceCodeTokenRequest(const std::string &client_id, const std::string &device_code) {
	return {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
	        {"client_id", client_id},
	        {"device_code", device_code}};
}

FormParams AuthorizationCodeTokenRequest(const std::string &client_id, const std::string &code,
                                         const std::string &redirect_uri, const std::string &code_verifier,
                                         const std::string &scope) {
	return {{"grant_type", "authorization_code"}, {"client_id", client_id},         {"code", code},
	        {"redirect_uri", redirect_uri},       {"code_verifier", code_verifier}, {"scope", scope}};
}

FormParams RefreshTokenRequest(const std::string &client_id, const std::string &client_secret,
                               const std::string &refresh_token, const std::string &scope) {
	FormParams p = {{"grant_type", "refresh_token"},
	                {"client_id", client_id},
	                {"refresh_token", refresh_token},
	                {"scope", scope}};
	// A public client must not send one; Entra rejects a secret it did not issue.
	if (!client_secret.empty()) {
		p.emplace_back("client_secret", client_secret);
	}
	return p;
}

bool ParseDeviceCodeResponse(const std::string &body, DeviceCodeGrant &out, std::string &error) {
	picojson::object o;
	if (!ParseObject(body, o)) {
		error = "the device-code response is not a JSON object";
		return false;
	}
	DeviceCodeGrant g;
	g.device_code = StringField(o, "device_code");
	g.user_code = StringField(o, "user_code");
	g.verification_uri = StringField(o, "verification_uri");
	g.expires_in = IntField(o, "expires_in");
	g.interval = IntField(o, "interval");
	if (g.device_code.empty() || g.user_code.empty() || g.verification_uri.empty()) {
		error = "the device-code response lacks device_code, user_code or verification_uri";
		return false;
	}
	// Both are shown to a human, on one line.
	if (!IsAllowedVerificationUri(g.verification_uri)) {
		error = "the device-code response's verification_uri is not an https Microsoft sign-in page";
		return false;
	}
	if (!IsValidUserCode(g.user_code)) {
		error = "the device-code response's user_code is not a short alphanumeric code";
		return false;
	}
	if (g.expires_in <= 0) {
		error = "the device-code response has no positive expires_in";
		return false;
	}
	out = g;
	return true;
}

TokenReply ParseTokenReply(int http_status, const std::string &body) {
	TokenReply r;
	picojson::object o;
	const bool parsed = ParseObject(body, o);
	if (http_status >= 200 && http_status < 300) {
		if (parsed) {
			r.access_token = StringField(o, "access_token");
			r.refresh_token = StringField(o, "refresh_token");
			r.expires_in = IntField(o, "expires_in");
		}
		r.kind = r.access_token.empty() ? TokenReplyKind::OTHER : TokenReplyKind::SUCCESS;
		return r;
	}
	if (parsed) {
		r.error = StringField(o, "error");
		auto codes = o.find("error_codes");
		if (codes != o.end() && codes->second.is<picojson::array>() &&
		    !codes->second.get<picojson::array>().empty() &&
		    codes->second.get<picojson::array>()[0].is<int64_t>()) {
			r.aadsts = codes->second.get<picojson::array>()[0].get<int64_t>();
		}
	}
	// AADSTS codes first: Entra reports consent and "not a public client" under
	// generic `error` values (invalid_grant / invalid_client).
	const std::string &e = r.error;
	if (r.aadsts == 65001 || e == "consent_required") {
		r.kind = TokenReplyKind::CONSENT_REQUIRED;
	} else if (r.aadsts == 7000218) {
		r.kind = TokenReplyKind::PUBLIC_CLIENT_DISABLED;
	} else if (e == "authorization_pending") {
		r.kind = TokenReplyKind::AUTHORIZATION_PENDING;
	} else if (e == "slow_down") {
		r.kind = TokenReplyKind::SLOW_DOWN;
	} else if (e == "expired_token" || e == "code_expired") {
		r.kind = TokenReplyKind::EXPIRED_TOKEN;
	} else if (e == "authorization_declined" || e == "access_denied") {
		r.kind = TokenReplyKind::DECLINED;
	} else if (e == "interaction_required" || e == "login_required") {
		r.kind = TokenReplyKind::INTERACTION_REQUIRED;
	} else if (e == "invalid_grant" || e == "bad_verification_code") {
		r.kind = TokenReplyKind::INVALID_GRANT;
	} else if (e == "invalid_client" || e == "unauthorized_client" || e == "invalid_scope" ||
	           e == "invalid_request") {
		r.kind = TokenReplyKind::INVALID_CLIENT;
	} else if (http_status == 429 || http_status >= 500) {
		r.kind = TokenReplyKind::TRANSIENT;
	} else {
		r.kind = TokenReplyKind::OTHER;
	}
	return r;
}

int64_t InitialPollIntervalSeconds(int64_t server_interval) {
	return server_interval <= 0 ? 5 : ClampInterval(server_interval);
}

int64_t NextPollIntervalSeconds(int64_t current, TokenReplyKind kind) {
	return ClampInterval(kind == TokenReplyKind::SLOW_DOWN ? current + 5 : current);
}

std::string DeviceCodePrompt(const DeviceCodeGrant &grant) {
	return "sharepoint: to sign in, open " + grant.verification_uri + " and enter the code " + grant.user_code +
	       " (valid for " + std::to_string(std::max<int64_t>(1, (grant.expires_in + 59) / 60)) + " minutes)";
}

std::string DelegatedErrorMessage(const TokenReply &reply, const std::string &secret_name,
                                  const std::string &provider) {
	std::string what;
	switch (reply.kind) {
	case TokenReplyKind::CONSENT_REQUIRED:
		what = "the app's delegated permissions have not been consented. An admin must grant consent for "
		       "Files.ReadWrite.All, Sites.ReadWrite.All and offline_access on the app registration "
		       "(Entra ID > App registrations > API permissions > Grant admin consent), or the user must accept "
		       "the consent prompt";
		break;
	case TokenReplyKind::EXPIRED_TOKEN:
		what = "the device code expired before the sign-in was completed. Run the query again for a new code";
		break;
	case TokenReplyKind::DECLINED:
		what = "the sign-in was declined";
		break;
	case TokenReplyKind::INTERACTION_REQUIRED:
	case TokenReplyKind::INVALID_GRANT:
		what = "the refresh token (or code) was rejected -- it is expired, revoked, or was issued to another app. "
		       "Recreate the secret without REFRESH_TOKEN (or with a fresh one) to sign in again";
		break;
	case TokenReplyKind::PUBLIC_CLIENT_DISABLED:
		what = "Entra ID asked for a client secret, so the app is not a public client. Enable 'Allow public client "
		       "flows' (Entra ID > App registrations > Authentication) -- delegated sign-in never sends a secret";
		break;
	case TokenReplyKind::INVALID_CLIENT:
		what = "Entra ID rejected the request. Check TENANT_ID and CLIENT_ID, that the app exists in that tenant, "
		       "that GRAPH_SCOPE is valid, and (authorization_code) that http://localhost is registered as a "
		       "'Mobile and desktop applications' redirect URI";
		break;
	case TokenReplyKind::TRANSIENT:
		what = "the Entra ID token endpoint is unavailable or throttling; try again shortly";
		break;
	default:
		what = "the Entra ID token endpoint returned an unexpected reply";
		break;
	}
	// The short `error` and the AADSTS number are enough to look a failure up;
	// error_description is never shown (it can echo configuration detail).
	std::string detail;
	if (!reply.error.empty() || reply.aadsts != 0) {
		detail = " (";
		detail += reply.error.empty() ? std::string("error") : reply.error;
		if (reply.aadsts != 0) {
			detail += " AADSTS" + std::to_string(reply.aadsts);
		}
		detail += ")";
	}
	return "sharepoint secret '" + secret_name + "' (PROVIDER " + provider + "): " + what + detail + ".";
}

bool ParseRedirectPort(const std::string &text, int &port) {
	if (text.empty()) {
		port = 8020;
		return true;
	}
	if (text.size() > 5) {
		return false;
	}
	int v = 0;
	for (char c : text) {
		if (c < '0' || c > '9') {
			return false;
		}
		v = v * 10 + (c - '0');
	}
	if (v < 1024 || v > 65535) {
		return false;
	}
	port = v;
	return true;
}

bool CanLaunchBrowser(bool is_posix_non_apple, const std::string &display, const std::string &wayland_display) {
	// macOS / Windows: `open` and ShellExecute work in any desktop session.
	// SSH_CONNECTION is deliberately not a veto: with X11 forwarding DISPLAY is
	// set and the browser opens on the user's own machine (as in gdrive).
	return !is_posix_non_apple || !display.empty() || !wayland_display.empty();
}

} // namespace sharepoint
} // namespace duckdb
