// PROVIDER credential_chain (#72), the decisions and parsing: see
// sharepoint_credential_chain.hpp.
#include "sharepoint_credential_chain.hpp"

#include <picojson.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace sharepoint {

namespace {

//! A token from az without expires_on is kept this long (old CLIs print only
//! a local-time expiresOn, which cannot be read safely).
constexpr int64_t kCliTokenSeconds = 300;

std::string UrlEncode(const std::string &s) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool Seconds(const picojson::object &o, const char *key, int64_t &out) {
	auto it = o.find(key);
	if (it == o.end()) {
		return false;
	}
	if (it->second.is<int64_t>()) {
		out = it->second.get<int64_t>();
		return true;
	}
	if (it->second.is<std::string>()) {
		char *end = nullptr;
		const auto &s = it->second.get<std::string>();
		const auto v = std::strtoll(s.c_str(), &end, 10);
		if (end && *end == '\0' && !s.empty()) {
			out = v;
			return true;
		}
	}
	return false;
}

std::string Field(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string();
}

} // namespace

const char *ChainSourceName(ChainSource source) {
	switch (source) {
	case ChainSource::WORKLOAD_IDENTITY:
		return "workload identity";
	case ChainSource::APP_SERVICE:
		return "managed identity (App Service)";
	case ChainSource::IMDS:
		return "managed identity (IMDS)";
	default:
		return "Azure CLI";
	}
}

std::vector<ChainSource> ChainOrder(const ChainEnv &env) {
	std::vector<ChainSource> order;
	if (!env.federated_token_file.empty() && !env.client_id.empty() && !env.tenant_id.empty()) {
		order.push_back(ChainSource::WORKLOAD_IDENTITY);
	}
	if (!env.identity_endpoint.empty() && !env.identity_header.empty()) {
		order.push_back(ChainSource::APP_SERVICE);
	} else {
		order.push_back(ChainSource::IMDS);
	}
	order.push_back(ChainSource::AZURE_CLI);
	return order;
}

std::string GraphResource(const std::string &graph_endpoint) {
	std::string r = graph_endpoint;
	while (!r.empty() && r.back() == '/') {
		r.pop_back();
	}
	return r;
}

std::string ManagedIdentityQuery(ChainSource source, const std::string &resource, const std::string &client_id) {
	std::string q = std::string("api-version=") + (source == ChainSource::APP_SERVICE ? "2019-08-01" : "2018-02-01") +
	                "&resource=" + UrlEncode(resource);
	if (!client_id.empty()) {
		q += "&client_id=" + UrlEncode(client_id);
	}
	return q;
}

ChainToken ParseManagedIdentityReply(int status, const std::string &body, int64_t now) {
	ChainToken t;
	picojson::value v;
	const bool json = picojson::parse(v, body).empty() && v.is<picojson::object>();
	if (status < 200 || status >= 300 || !json) {
		std::string why = json ? Field(v.get<picojson::object>(), "error_description") : std::string();
		if (why.empty() && json) {
			why = Field(v.get<picojson::object>(), "error");
		}
		t.error = "HTTP " + std::to_string(status) + (why.empty() ? std::string() : ": " + why.substr(0, 200));
		return t;
	}
	const auto &o = v.get<picojson::object>();
	t.token = Field(o, "access_token");
	if (t.token.empty()) {
		t.error = "no access_token in the reply";
		return t;
	}
	int64_t s;
	if (Seconds(o, "expires_on", s)) {
		t.expires_on = s;
	} else if (Seconds(o, "expires_in", s)) {
		t.expires_on = now + s;
	} else {
		t.expires_on = now + kCliTokenSeconds;
	}
	t.ok = true;
	return t;
}

ChainToken ParseAzureCliReply(const std::string &output, int64_t now) {
	ChainToken t;
	picojson::value v;
	if (!picojson::parse(v, output).empty() || !v.is<picojson::object>()) {
		// az's own message ("Please run 'az login'..."), never a token.
		auto msg = output.substr(0, 300);
		while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
			msg.pop_back();
		}
		t.error = msg.empty() ? "az printed nothing" : msg;
		return t;
	}
	const auto &o = v.get<picojson::object>();
	t.token = Field(o, "accessToken");
	if (t.token.empty()) {
		t.error = "no accessToken in az's output";
		return t;
	}
	int64_t s;
	t.expires_on = Seconds(o, "expires_on", s) ? s : now + kCliTokenSeconds;
	t.ok = true;
	return t;
}

} // namespace sharepoint
} // namespace duckdb
