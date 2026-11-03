// The client assertion's shape (#71). Pure: see sharepoint_assertion.hpp.
#include "sharepoint_assertion.hpp"

namespace duckdb {
namespace sharepoint {

namespace {

//! An assertion is good for this long; Entra wants it short-lived.
constexpr int64_t kAssertionLifetimeSeconds = 600;

std::string JsonString(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			static const char *hex = "0123456789abcdef";
			out += "\\u00";
			out += hex[(c >> 4) & 0xF];
			out += hex[c & 0xF];
		} else {
			out += c;
		}
	}
	return out + "\"";
}

} // namespace

std::string Base64Url(const std::string &bytes) {
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	size_t i = 0;
	for (; i + 2 < bytes.size(); i += 3) {
		const uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
		                   (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
		out += alphabet[(n >> 18) & 63];
		out += alphabet[(n >> 12) & 63];
		out += alphabet[(n >> 6) & 63];
		out += alphabet[n & 63];
	}
	if (i + 1 == bytes.size()) {
		const uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
		out += alphabet[(n >> 18) & 63];
		out += alphabet[(n >> 12) & 63];
	} else if (i + 2 == bytes.size()) {
		const uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
		out += alphabet[(n >> 18) & 63];
		out += alphabet[(n >> 12) & 63];
		out += alphabet[(n >> 6) & 63];
	}
	return out;
}

std::string ClientAssertionSigningInput(const std::string &client_id, const std::string &audience,
                                        const std::string &x5t, const std::string &jti, int64_t now) {
	const std::string header = R"({"alg":"RS256","typ":"JWT","x5t":)" + JsonString(x5t) + "}";
	const std::string claims = "{\"aud\":" + JsonString(audience) + ",\"iss\":" + JsonString(client_id) +
	                           ",\"sub\":" + JsonString(client_id) + ",\"jti\":" + JsonString(jti) +
	                           ",\"nbf\":" + std::to_string(now) + ",\"iat\":" + std::to_string(now) +
	                           ",\"exp\":" + std::to_string(now + kAssertionLifetimeSeconds) + "}";
	return Base64Url(header) + "." + Base64Url(claims);
}

} // namespace sharepoint
} // namespace duckdb
