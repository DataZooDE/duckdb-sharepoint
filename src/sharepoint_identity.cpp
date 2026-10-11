#include "sharepoint_identity.hpp"

#include <cstddef>
#include <initializer_list>

namespace duckdb {
namespace sharepoint {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

// The lowercased host of "https://<host>[/]", or empty if `url` has any other
// shape (scheme, port, path, query, userinfo).
std::string HttpsBareHost(const std::string &url) {
	static const std::string kScheme = "https://";
	std::string u = Lower(url);
	if (u.compare(0, kScheme.size(), kScheme) != 0) {
		return std::string();
	}
	std::string host = u.substr(kScheme.size());
	if (!host.empty() && host.back() == '/') {
		host.pop_back();
	}
	if (host.empty() || host.find_first_of("/:?#@[] ") != std::string::npos) {
		return std::string();
	}
	return host;
}

bool HostIn(const std::string &host, std::initializer_list<const char *> allowed) {
	if (host.empty()) {
		return false;
	}
	for (const char *a : allowed) {
		if (host == a) {
			return true;
		}
	}
	return false;
}

} // namespace

bool IsAllowedGraphEndpoint(const std::string &url) {
	return HostIn(HttpsBareHost(url), {"graph.microsoft.com", "graph.microsoft.us", "dod-graph.microsoft.us",
	                                   "microsoftgraph.chinacloudapi.cn"});
}

bool IsAllowedLoginEndpoint(const std::string &url) {
	return HostIn(HttpsBareHost(url), {"login.microsoftonline.com", "login.microsoftonline.us",
	                                   "login.partner.microsoftonline.cn", "login.chinacloudapi.cn"});
}

bool IsValidTenantId(const std::string &tenant) {
	if (tenant.empty() || tenant == "." || tenant == "..") {
		return false;
	}
	for (char c : tenant) {
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
			return false;
		}
	}
	return true;
}

std::string FrameCredentialMaterial(const std::vector<std::string> &parts) {
	std::string out;
	for (const auto &p : parts) {
		out += std::to_string(p.size());
		out += ':';
		out += p;
		out += ';';
	}
	return out;
}

std::string DefaultGraphScope(const std::string &graph_endpoint) {
	std::string base = graph_endpoint;
	while (!base.empty() && base.back() == '/') {
		base.pop_back();
	}
	return base + "/.default";
}

} // namespace sharepoint
} // namespace duckdb
